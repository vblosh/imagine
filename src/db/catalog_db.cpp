#include "imagine/db/catalog_db.hpp"
#include "imagine/core/reverse_geocoder.hpp"
#include "imagine/db/schema.hpp"
#include "imagine/common/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <unordered_set>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace imagine::db {

static Status invalidateFaceAnalysis(Connection& conn, MediaId mediaId);
static Result<bool> mediaFaceGeometryChanged(Connection& conn, const MediaItem& item);

static constexpr const char* kMediaColumnsProjection =
    "id, file_path, file_name, file_size, file_modified_time, content_hash, "
    "width, height, date_taken, date_taken_str, rating, flag, "
    "camera_make, camera_model, lens, exposure_time, f_number, iso, "
    "focal_length, orientation, has_gps, latitude, longitude, altitude, "
    "thumb_small, thumb_large, created_at, updated_at, caption, "
    "media_type, duration, audio_artist, audio_title, audio_album, audio_genre, "
    "codec, bitrate, channels, sample_rate";

static int64_t currentUnixTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return target.parent_path();
#else
    return {};
#endif
}

static bool validGpsCoordinates(double latitude, double longitude) {
    return std::isfinite(latitude) && std::isfinite(longitude) &&
           latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

static void logGeocoderUnavailableThrottled() {
    static std::mutex warningMutex;
    static int64_t lastWarningTime = 0;
    const int64_t now = currentUnixTime();
    std::lock_guard<std::mutex> lock(warningMutex);
    if (lastWarningTime != 0 && now - lastWarningTime < 60) {
        return;
    }
    lastWarningTime = now;
    IMAGINE_LOG_WARN("Skipping GPS place tag update: reverse geocoder unavailable (GeoNames dataset missing or invalid)");
}

static std::string findGeonamesDataset() {
    if (const char* envPath = std::getenv("IMAGINE_GEONAMES_CSV"); envPath != nullptr && *envPath != '\0') {
        return envPath;
    }

    std::vector<std::filesystem::path> searchDirs;
    const auto exeDir = executableDirectory();
    if (!exeDir.empty()) {
        searchDirs.push_back(exeDir);
    }
    std::error_code cwdEc;
    const auto cwd = std::filesystem::current_path(cwdEc);
    if (!cwdEc) {
        searchDirs.push_back(cwd);
    }

    for (const auto& dir : searchDirs) {
        for (const auto& relative : {
                 std::filesystem::path("geonames_cities15000.csv"),
                 std::filesystem::path("data") / "geonames_cities15000.csv"}) {
            std::error_code ec;
            const auto candidate = dir / relative;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate.string();
            }
        }
    }
    return {};
}

std::string CatalogDb::geonamesDatasetInfo() const {
    return findGeonamesDataset();
}

CatalogDb::CatalogDb() = default;
CatalogDb::~CatalogDb() {
    close();
}

Status CatalogDb::open(const std::string& dbPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Status s = conn_.open(dbPath);
    if (!s.isOk()) return s;
    return Schema::migrate(conn_);
}

void CatalogDb::close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    conn_.close();
}

bool CatalogDb::isOpen() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return conn_.isOpen();
}

MediaItem CatalogDb::extractMediaItem(Statement& stmt) {
    MediaItem m;
    m.id = stmt.getInt64(0);
    m.file_path = stmt.getString(1);
    m.file_name = stmt.getString(2);
    m.file_size = stmt.getInt64(3);
    m.file_modified_time = stmt.getInt64(4);
    m.content_hash = stmt.getString(5);
    m.width = stmt.getInt(6);
    m.height = stmt.getInt(7);
    m.date_taken = stmt.getInt64(8);
    m.exif.date_taken = m.date_taken;
    m.exif.date_taken_str = stmt.getString(9);
    m.rating = stmt.getInt(10);
    m.flag = static_cast<FlagState>(stmt.getInt(11));
    m.exif.camera_make = stmt.getString(12);
    m.exif.camera_model = stmt.getString(13);
    m.exif.lens = stmt.getString(14);
    m.exif.exposure_time = stmt.getDouble(15);
    m.exif.f_number = stmt.getDouble(16);
    m.exif.iso = stmt.getInt(17);
    m.exif.focal_length = stmt.getDouble(18);
    m.exif.orientation = stmt.getInt(19);
    m.exif.has_gps = (stmt.getInt(20) != 0);
    m.exif.latitude = stmt.getDouble(21);
    m.exif.longitude = stmt.getDouble(22);
    m.exif.altitude = stmt.getDouble(23);
    m.thumb_small = stmt.getString(24);
    m.thumb_large = stmt.getString(25);
    m.created_at = stmt.getInt64(26);
    m.updated_at = stmt.getInt64(27);
    if (stmt.columnCount() > 28) {
        m.caption = stmt.getString(28);
    }
    if (stmt.columnCount() > 29) {
        m.media_type = stmt.getString(29);
        m.duration = stmt.getDouble(30);
        m.audio_artist = stmt.getString(31);
        m.audio_title = stmt.getString(32);
        m.audio_album = stmt.getString(33);
        m.audio_genre = stmt.getString(34);
        m.codec = stmt.getString(35);
        m.bitrate = stmt.getInt(36);
        m.channels = stmt.getInt(37);
        m.sample_rate = stmt.getInt(38);
    }
    return m;
}

Result<MediaId> CatalogDb::insertMedia(MediaItem& item) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        INSERT INTO media_items (
            file_path, file_name, file_size, file_modified_time, content_hash,
            width, height, date_taken, date_taken_str, rating, flag,
            camera_make, camera_model, lens, exposure_time, f_number, iso,
            focal_length, orientation, has_gps, latitude, longitude, altitude,
            thumb_small, thumb_large, created_at, updated_at, caption,
            media_type, duration, audio_artist, audio_title, audio_album, audio_genre,
            codec, bitrate, channels, sample_rate
        ) VALUES (
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?
        );
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    int64_t now = currentUnixTime();
    item.created_at = now;
    item.updated_at = now;

    stmt.bind(1, item.file_path);
    stmt.bind(2, item.file_name);
    stmt.bind(3, item.file_size);
    stmt.bind(4, item.file_modified_time);
    stmt.bind(5, item.content_hash);
    stmt.bind(6, item.width);
    stmt.bind(7, item.height);
    stmt.bind(8, item.date_taken);
    stmt.bind(9, item.exif.date_taken_str);
    stmt.bind(10, item.rating);
    stmt.bind(11, static_cast<int32_t>(item.flag));
    stmt.bind(12, item.exif.camera_make);
    stmt.bind(13, item.exif.camera_model);
    stmt.bind(14, item.exif.lens);
    stmt.bind(15, item.exif.exposure_time);
    stmt.bind(16, item.exif.f_number);
    stmt.bind(17, item.exif.iso);
    stmt.bind(18, item.exif.focal_length);
    stmt.bind(19, item.exif.orientation);
    stmt.bind(20, item.exif.has_gps ? 1 : 0);
    stmt.bind(21, item.exif.latitude);
    stmt.bind(22, item.exif.longitude);
    stmt.bind(23, item.exif.altitude);
    stmt.bind(24, item.thumb_small);
    stmt.bind(25, item.thumb_large);
    stmt.bind(26, item.created_at);
    stmt.bind(27, item.updated_at);
    stmt.bind(28, item.caption);
    stmt.bind(29, item.media_type);
    stmt.bind(30, item.duration);
    stmt.bind(31, item.audio_artist);
    stmt.bind(32, item.audio_title);
    stmt.bind(33, item.audio_album);
    stmt.bind(34, item.audio_genre);
    stmt.bind(35, item.codec);
    stmt.bind(36, item.bitrate);
    stmt.bind(37, item.channels);
    stmt.bind(38, item.sample_rate);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to insert media item: " + conn_.lastErrorMessage());
    }

    item.id = conn_.lastInsertRowId();
    return item.id;
}

Result<size_t> CatalogDb::insertMediaBatch(std::vector<MediaItem>& items) {
    if (items.empty()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const char* sql = R"SQL(
        INSERT INTO media_items (
            file_path, file_name, file_size, file_modified_time, content_hash,
            width, height, date_taken, date_taken_str, rating, flag,
            camera_make, camera_model, lens, exposure_time, f_number, iso,
            focal_length, orientation, has_gps, latitude, longitude, altitude,
            thumb_small, thumb_large, created_at, updated_at, caption,
            media_type, duration, audio_artist, audio_title, audio_album, audio_genre,
            codec, bitrate, channels, sample_rate
        ) VALUES (
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?
        );
    )SQL";

    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    int64_t now = currentUnixTime();
    size_t insertedCount = 0;

    for (auto& item : items) {
        item.created_at = now;
        item.updated_at = now;

        stmt.bind(1, item.file_path);
        stmt.bind(2, item.file_name);
        stmt.bind(3, item.file_size);
        stmt.bind(4, item.file_modified_time);
        stmt.bind(5, item.content_hash);
        stmt.bind(6, item.width);
        stmt.bind(7, item.height);
        stmt.bind(8, item.date_taken);
        stmt.bind(9, item.exif.date_taken_str);
        stmt.bind(10, item.rating);
        stmt.bind(11, static_cast<int32_t>(item.flag));
        stmt.bind(12, item.exif.camera_make);
        stmt.bind(13, item.exif.camera_model);
        stmt.bind(14, item.exif.lens);
        stmt.bind(15, item.exif.exposure_time);
        stmt.bind(16, item.exif.f_number);
        stmt.bind(17, item.exif.iso);
        stmt.bind(18, item.exif.focal_length);
        stmt.bind(19, item.exif.orientation);
        stmt.bind(20, item.exif.has_gps ? 1 : 0);
        stmt.bind(21, item.exif.latitude);
        stmt.bind(22, item.exif.longitude);
        stmt.bind(23, item.exif.altitude);
        stmt.bind(24, item.thumb_small);
        stmt.bind(25, item.thumb_large);
        stmt.bind(26, item.created_at);
        stmt.bind(27, item.updated_at);
        stmt.bind(28, item.caption);
        stmt.bind(29, item.media_type);
        stmt.bind(30, item.duration);
        stmt.bind(31, item.audio_artist);
        stmt.bind(32, item.audio_title);
        stmt.bind(33, item.audio_album);
        stmt.bind(34, item.audio_genre);
        stmt.bind(35, item.codec);
        stmt.bind(36, item.bitrate);
        stmt.bind(37, item.channels);
        stmt.bind(38, item.sample_rate);

        if (stmt.step() != StepResult::Done) {
            return Status::databaseError("Failed to insert media item in batch: " + conn_.lastErrorMessage());
        }

        item.id = conn_.lastInsertRowId();
        insertedCount++;
        stmt.reset();
    }

    Status commitStatus = tx.commit();
    if (!commitStatus.isOk()) {
        return commitStatus;
    }

    return insertedCount;
}

Status CatalogDb::updateMedia(const MediaItem& item) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto geometryChangedRes = mediaFaceGeometryChanged(conn_, item);
    if (!geometryChangedRes.isOk()) return geometryChangedRes.status();
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    const char* sql = R"SQL(
        UPDATE media_items SET
            file_path = ?, file_name = ?, file_size = ?, file_modified_time = ?, content_hash = ?,
            width = ?, height = ?, date_taken = ?, date_taken_str = ?,
            rating = ?, flag = ?, camera_make = ?, camera_model = ?, lens = ?,
            exposure_time = ?, f_number = ?, iso = ?, focal_length = ?,
            orientation = ?, has_gps = ?, latitude = ?, longitude = ?, altitude = ?,
            thumb_small = ?, thumb_large = ?, updated_at = ?, caption = ?,
            media_type = ?, duration = ?, audio_artist = ?, audio_title = ?, audio_album = ?,
            audio_genre = ?, codec = ?, bitrate = ?, channels = ?, sample_rate = ?
        WHERE id = ?;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    int64_t now = currentUnixTime();
    stmt.bind(1, item.file_path);
    stmt.bind(2, item.file_name);
    stmt.bind(3, item.file_size);
    stmt.bind(4, item.file_modified_time);
    stmt.bind(5, item.content_hash);
    stmt.bind(6, item.width);
    stmt.bind(7, item.height);
    stmt.bind(8, item.date_taken);
    stmt.bind(9, item.exif.date_taken_str);
    stmt.bind(10, item.rating);
    stmt.bind(11, static_cast<int32_t>(item.flag));
    stmt.bind(12, item.exif.camera_make);
    stmt.bind(13, item.exif.camera_model);
    stmt.bind(14, item.exif.lens);
    stmt.bind(15, item.exif.exposure_time);
    stmt.bind(16, item.exif.f_number);
    stmt.bind(17, item.exif.iso);
    stmt.bind(18, item.exif.focal_length);
    stmt.bind(19, item.exif.orientation);
    stmt.bind(20, item.exif.has_gps ? 1 : 0);
    stmt.bind(21, item.exif.latitude);
    stmt.bind(22, item.exif.longitude);
    stmt.bind(23, item.exif.altitude);
    stmt.bind(24, item.thumb_small);
    stmt.bind(25, item.thumb_large);
    stmt.bind(26, now);
    stmt.bind(27, item.caption);
    stmt.bind(28, item.media_type);
    stmt.bind(29, item.duration);
    stmt.bind(30, item.audio_artist);
    stmt.bind(31, item.audio_title);
    stmt.bind(32, item.audio_album);
    stmt.bind(33, item.audio_genre);
    stmt.bind(34, item.codec);
    stmt.bind(35, item.bitrate);
    stmt.bind(36, item.channels);
    stmt.bind(37, item.sample_rate);
    stmt.bind(38, item.id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update media item: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(item.id));
    }
    if (geometryChangedRes.value()) {
        Status invalidated = invalidateFaceAnalysis(conn_, item.id);
        if (!invalidated.isOk()) return invalidated;
    }
    return tx.commit();
}

Result<size_t> CatalogDb::updateMediaBatch(const std::vector<MediaItem>& items) {
    if (items.empty()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const char* sql = R"SQL(
        UPDATE media_items SET
            file_path = ?, file_name = ?, file_size = ?, file_modified_time = ?, content_hash = ?,
            width = ?, height = ?, date_taken = ?, date_taken_str = ?,
            rating = ?, flag = ?, camera_make = ?, camera_model = ?, lens = ?,
            exposure_time = ?, f_number = ?, iso = ?, focal_length = ?,
            orientation = ?, has_gps = ?, latitude = ?, longitude = ?, altitude = ?,
            thumb_small = ?, thumb_large = ?, updated_at = ?, caption = ?,
            media_type = ?, duration = ?, audio_artist = ?, audio_title = ?, audio_album = ?,
            audio_genre = ?, codec = ?, bitrate = ?, channels = ?, sample_rate = ?
        WHERE id = ?;
    )SQL";

    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto geomStmtRes = conn_.prepare("SELECT content_hash,width,height,orientation,media_type FROM media_items WHERE id=?;");
    if (!geomStmtRes.isOk()) return geomStmtRes.status();
    auto geomStmt = std::move(geomStmtRes.value());

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    int64_t now = currentUnixTime();
    size_t updatedCount = 0;

    for (const auto& item : items) {
        geomStmt.reset();
        geomStmt.bind(1, item.id);
        bool geometryChanged = false;
        if (geomStmt.step() == StepResult::Row) {
            geometryChanged = (geomStmt.getString(0) != item.content_hash ||
                               geomStmt.getInt(1) != item.width ||
                               geomStmt.getInt(2) != item.height ||
                               geomStmt.getInt(3) != item.exif.orientation ||
                               geomStmt.getString(4) != item.media_type);
        }

        stmt.bind(1, item.file_path);
        stmt.bind(2, item.file_name);
        stmt.bind(3, item.file_size);
        stmt.bind(4, item.file_modified_time);
        stmt.bind(5, item.content_hash);
        stmt.bind(6, item.width);
        stmt.bind(7, item.height);
        stmt.bind(8, item.date_taken);
        stmt.bind(9, item.exif.date_taken_str);
        stmt.bind(10, item.rating);
        stmt.bind(11, static_cast<int32_t>(item.flag));
        stmt.bind(12, item.exif.camera_make);
        stmt.bind(13, item.exif.camera_model);
        stmt.bind(14, item.exif.lens);
        stmt.bind(15, item.exif.exposure_time);
        stmt.bind(16, item.exif.f_number);
        stmt.bind(17, item.exif.iso);
        stmt.bind(18, item.exif.focal_length);
        stmt.bind(19, item.exif.orientation);
        stmt.bind(20, item.exif.has_gps ? 1 : 0);
        stmt.bind(21, item.exif.latitude);
        stmt.bind(22, item.exif.longitude);
        stmt.bind(23, item.exif.altitude);
        stmt.bind(24, item.thumb_small);
        stmt.bind(25, item.thumb_large);
        stmt.bind(26, now);
        stmt.bind(27, item.caption);
        stmt.bind(28, item.media_type);
        stmt.bind(29, item.duration);
        stmt.bind(30, item.audio_artist);
        stmt.bind(31, item.audio_title);
        stmt.bind(32, item.audio_album);
        stmt.bind(33, item.audio_genre);
        stmt.bind(34, item.codec);
        stmt.bind(35, item.bitrate);
        stmt.bind(36, item.channels);
        stmt.bind(37, item.sample_rate);
        stmt.bind(38, item.id);

        if (stmt.step() != StepResult::Done) {
            return Status::databaseError("Failed to update media item in batch: " + conn_.lastErrorMessage());
        }
        if (conn_.changes() > 0) {
            updatedCount++;
        }
        stmt.reset();
        if (geometryChanged) {
            Status invalidated = invalidateFaceAnalysis(conn_, item.id);
            if (!invalidated.isOk()) return invalidated;
        }
    }

    Status commitStatus = tx.commit();
    if (!commitStatus.isOk()) {
        return commitStatus;
    }

    return updatedCount;
}

Status CatalogDb::updateCaption(MediaId id, const std::string& caption) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET caption = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, caption);
    stmt.bind(2, currentUnixTime());
    stmt.bind(3, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update caption: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

Status CatalogDb::updateFileNameAndPath(MediaId id, const std::string& newFileName, const std::string& newFilePath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET file_name = ?, file_path = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, newFileName);
    stmt.bind(2, newFilePath);
    stmt.bind(3, currentUnixTime());
    stmt.bind(4, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update file name and path: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

Status CatalogDb::updateDateTaken(MediaId id, int64_t dateTaken, const std::string& dateTakenStr) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET date_taken = ?, date_taken_str = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, dateTaken);
    stmt.bind(2, dateTakenStr);
    stmt.bind(3, currentUnixTime());
    stmt.bind(4, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update date taken: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}


Result<MediaItem> CatalogDb::getMediaById(MediaId id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kMediaColumnsProjection + " FROM media_items WHERE id = ?;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);

    if (stmt.step() == StepResult::Row) {
        auto m = extractMediaItem(stmt);
        // fetch tags
        auto tagsRes = getTagsForMedia(id);
        if (tagsRes.isOk()) {
            m.tags = tagsRes.value();
        }
        return m;
    }
    return Status::notFound("Media item not found with id " + std::to_string(id));
}

Result<MediaItem> CatalogDb::getMediaByPath(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kMediaColumnsProjection + " FROM media_items WHERE file_path = ?;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, path);

    if (stmt.step() == StepResult::Row) {
        auto m = extractMediaItem(stmt);
        auto tagsRes = getTagsForMedia(m.id);
        if (tagsRes.isOk()) {
            m.tags = tagsRes.value();
        }
        return m;
    }
    return Status::notFound("Media item not found with path: " + path);
}

Result<MediaItem> CatalogDb::getMediaByHash(const std::string& hash) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kMediaColumnsProjection + " FROM media_items WHERE content_hash = ? LIMIT 1;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, hash);

    if (stmt.step() == StepResult::Row) {
        auto m = extractMediaItem(stmt);
        auto tagsRes = getTagsForMedia(m.id);
        if (tagsRes.isOk()) {
            m.tags = tagsRes.value();
        }
        return m;
    }
    return Status::notFound("Media item not found with hash: " + hash);
}

Status CatalogDb::updateRating(MediaId id, int32_t rating) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET rating = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, rating);
    stmt.bind(2, currentUnixTime());
    stmt.bind(3, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update rating: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

Status CatalogDb::updateFlag(MediaId id, FlagState flag) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET flag = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, static_cast<int32_t>(flag));
    stmt.bind(2, currentUnixTime());
    stmt.bind(3, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update flag: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

Status CatalogDb::updateThumbnails(MediaId id, const std::string& smallPath, const std::string& largePath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET thumb_small = ?, thumb_large = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, smallPath);
    stmt.bind(2, largePath);
    stmt.bind(3, currentUnixTime());
    stmt.bind(4, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update thumbnails: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

Status CatalogDb::updateGps(MediaId id, bool hasGps, double latitude, double longitude, double altitude) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("UPDATE media_items SET has_gps = ?, latitude = ?, longitude = ?, altitude = ?, updated_at = ? WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, hasGps ? 1 : 0);
    stmt.bind(2, latitude);
    stmt.bind(3, longitude);
    stmt.bind(4, altitude);
    stmt.bind(5, currentUnixTime());
    stmt.bind(6, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to update GPS coordinates: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

bool CatalogDb::ensureGeocoderLoaded() {
    std::lock_guard<std::mutex> lock(geocoderMutex_);
    if (geocoderLoadAttempted_) {
        return geocoderAvailable_;
    }
    geocoderLoadAttempted_ = true;

    const std::string datasetPath = findGeonamesDataset();
    if (datasetPath.empty()) {
        IMAGINE_LOG_WARN("GPS place tagging disabled: GeoNames dataset not found (set IMAGINE_GEONAMES_CSV to override)");
        return false;
    }

    auto geocoder = std::make_unique<core::ReverseGeocoder>();
    if (!geocoder->load(datasetPath)) {
        IMAGINE_LOG_WARN("GPS place tagging disabled: failed to load GeoNames dataset: " + datasetPath);
        return false;
    }

    geocoder_ = std::move(geocoder);
    geocoderAvailable_ = true;
    return true;
}

Status CatalogDb::applyGpsPlaceTags(MediaId id, bool hasGps, double latitude, double longitude) {
    const bool coordinatesValid = validGpsCoordinates(latitude, longitude);
    std::vector<std::string> names;

    if (hasGps && coordinatesValid) {
        if (!ensureGeocoderLoaded()) {
            logGeocoderUnavailableThrottled();
            return Status::ok();
        }

        core::ReverseGeocoder* geocoder = nullptr;
        {
            std::lock_guard<std::mutex> lock(geocoderMutex_);
            geocoder = geocoder_.get();
        }
        if (geocoder == nullptr) {
            logGeocoderUnavailableThrottled();
            return Status::ok();
        }

        if (const auto place = geocoder->lookup(latitude, longitude); place.has_value()) {
            const std::string* candidates[] = {
                &place->city,
                &place->admin1,
                &place->countryName,
            };
            for (const std::string* candidate : candidates) {
                if (candidate->empty()) {
                    continue;
                }
                if (std::find(names.begin(), names.end(), *candidate) == names.end()) {
                    names.push_back(*candidate);
                }
            }
        }
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    std::vector<TagId> tagIds;
    tagIds.reserve(names.size());
    for (const auto& name : names) {
        auto tagRes = createOrGetTag(name, "places");
        if (!tagRes.isOk()) return tagRes.status();
        tagIds.push_back(tagRes.value());
    }

    std::string placeholders;
    for (size_t i = 0; i < tagIds.size(); ++i) {
        if (i > 0) placeholders += ",";
        placeholders += "?";
    }
    std::string staleWhere = "media_id = ? AND source = 'gps'";
    if (!placeholders.empty()) {
        staleWhere += " AND tag_id NOT IN (" + placeholders + ")";
    }

    auto clearLinksRes = conn_.prepare(
        "DELETE FROM media_tags WHERE media_id = ? AND tag_id IN ("
        "SELECT tag_id FROM media_tag_provenance WHERE " + staleWhere + ");");
    if (!clearLinksRes.isOk()) return clearLinksRes.status();
    auto clearLinks = std::move(clearLinksRes.value());
    clearLinks.bind(1, id);
    clearLinks.bind(2, id);
    for (size_t i = 0; i < tagIds.size(); ++i) {
        clearLinks.bind(static_cast<int>(i + 3), tagIds[i]);
    }
    if (clearLinks.step() != StepResult::Done) {
        return Status::databaseError("Failed to clear stale GPS place links: " + conn_.lastErrorMessage());
    }

    auto clearProvenanceRes = conn_.prepare(
        "DELETE FROM media_tag_provenance WHERE " + staleWhere + ";");
    if (!clearProvenanceRes.isOk()) return clearProvenanceRes.status();
    auto clearProvenance = std::move(clearProvenanceRes.value());
    clearProvenance.bind(1, id);
    for (size_t i = 0; i < tagIds.size(); ++i) {
        clearProvenance.bind(static_cast<int>(i + 2), tagIds[i]);
    }
    if (clearProvenance.step() != StepResult::Done) {
        return Status::databaseError("Failed to clear stale GPS place provenance: " + conn_.lastErrorMessage());
    }

    auto insertLinkRes = conn_.prepare("INSERT OR IGNORE INTO media_tags (media_id, tag_id) VALUES (?, ?);");
    if (!insertLinkRes.isOk()) return insertLinkRes.status();
    auto insertLink = std::move(insertLinkRes.value());

    auto insertProvenanceRes = conn_.prepare(
        "INSERT INTO media_tag_provenance(media_id, tag_id, manual, source) VALUES(?, ?, 0, 'gps') "
        "ON CONFLICT(media_id, tag_id) DO UPDATE SET manual = 0, source = 'gps' "
        "WHERE media_tag_provenance.source = 'gps';");
    if (!insertProvenanceRes.isOk()) return insertProvenanceRes.status();
    auto insertProvenance = std::move(insertProvenanceRes.value());

    for (const TagId tagId : tagIds) {
        insertLink.reset();
        insertLink.bind(1, id);
        insertLink.bind(2, tagId);
        if (insertLink.step() != StepResult::Done) {
            return Status::databaseError("Failed to add GPS place tag: " + conn_.lastErrorMessage());
        }

        insertProvenance.reset();
        insertProvenance.bind(1, id);
        insertProvenance.bind(2, tagId);
        if (insertProvenance.step() != StepResult::Done) {
            return Status::databaseError("Failed to record GPS place tag provenance: " + conn_.lastErrorMessage());
        }
    }

    return tx.commit();
}

Status CatalogDb::deleteMedia(MediaId id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("DELETE FROM media_items WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to delete media item: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Media item not found: " + std::to_string(id));
    }
    return Status::ok();
}

// --- Tags ---

Result<TagId> CatalogDb::createOrGetTag(const std::string& name, const std::string& category, std::optional<TagId> parent_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto checkRes = conn_.prepare("SELECT id FROM tags WHERE name = ? AND category = ?;");
    if (checkRes.isOk()) {
        auto checkStmt = std::move(checkRes.value());
        checkStmt.bind(1, name);
        checkStmt.bind(2, category);
        if (checkStmt.step() == StepResult::Row) {
            return checkStmt.getInt64(0);
        }
    }

    auto insRes = conn_.prepare("INSERT INTO tags (name, category, parent_id) VALUES (?, ?, ?);");
    if (!insRes.isOk()) return insRes.status();
    auto insStmt = std::move(insRes.value());
    insStmt.bind(1, name);
    insStmt.bind(2, category);
    insStmt.bindOptional(3, parent_id);

    if (insStmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to create tag: " + conn_.lastErrorMessage());
    }
    return conn_.lastInsertRowId();
}

Result<std::vector<Tag>> CatalogDb::getAllTags() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        WITH tag_stats AS (
            SELECT mt.tag_id,
                   COUNT(mt.media_id) AS media_count,
                   MIN(CASE WHEN m.date_taken != 0 THEN m.date_taken WHEN m.created_at > 0 THEN m.created_at WHEN m.file_modified_time > 0 THEN m.file_modified_time ELSE NULL END) AS first_date,
                   MAX(CASE WHEN m.date_taken != 0 THEN m.date_taken WHEN m.created_at > 0 THEN m.created_at WHEN m.file_modified_time > 0 THEN m.file_modified_time ELSE NULL END) AS last_date
            FROM media_tags mt
            JOIN media_items m ON mt.media_id = m.id
            GROUP BY mt.tag_id
        ),
        tag_ai_stats AS (
            SELECT p.tag_id, COUNT(*) AS ai_count
            FROM media_tag_provenance p
            JOIN media_tags mt ON p.media_id = mt.media_id AND p.tag_id = mt.tag_id
            WHERE p.source = 'clip'
            GROUP BY p.tag_id
        ),
        tag_covers AS (
            SELECT t.id AS tag_id,
                   (SELECT mt2.media_id
                    FROM media_tags mt2
                    JOIN media_items m2 ON mt2.media_id = m2.id
                    WHERE mt2.tag_id = t.id
                    ORDER BY (CASE WHEN m2.media_type IN ('video', 'audio') THEN 1 ELSE 0 END) ASC,
                             m2.date_taken DESC, m2.id DESC
                    LIMIT 1) AS cover_media_id
            FROM tags t
            WHERE EXISTS (SELECT 1 FROM media_tags mt3 WHERE mt3.tag_id = t.id)
        )
        SELECT t.id, t.name, t.category, t.parent_id,
               COALESCE(ts.media_count, 0) AS media_count,
               tc.cover_media_id,
               cov.content_hash AS cover_hash,
               ts.first_date,
               ts.last_date,
               COALESCE(ai.ai_count, 0) AS ai_count
        FROM tags t
        LEFT JOIN tag_stats ts ON t.id = ts.tag_id
        LEFT JOIN tag_ai_stats ai ON t.id = ai.tag_id
        LEFT JOIN tag_covers tc ON t.id = tc.tag_id
        LEFT JOIN media_items cov ON tc.cover_media_id = cov.id
        ORDER BY t.category ASC, t.name ASC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::vector<Tag> tags;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        Tag t;
        t.id = stmt.getInt64(0);
        t.name = stmt.getString(1);
        t.category = stmt.getString(2);
        if (!stmt.isNull(3)) {
            t.parent_id = stmt.getInt64(3);
        }
        t.media_count = stmt.getInt64(4);
        if (!stmt.isNull(5)) {
            t.cover_media_id = stmt.getInt64(5);
        }
        if (!stmt.isNull(6)) {
            t.cover_hash = stmt.getString(6);
        }
        if (!stmt.isNull(7)) {
            t.first_date = stmt.getInt64(7);
        }
        if (!stmt.isNull(8)) {
            t.last_date = stmt.getInt64(8);
        }
        t.ai_count = stmt.getInt64(9);
        tags.push_back(std::move(t));
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read tags: " + conn_.lastErrorMessage());
    }
    return tags;
}

Result<std::vector<Tag>> CatalogDb::getTagsForMedia(MediaId id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        SELECT t.id, t.name, t.category, t.parent_id, COALESCE(mtp.source, 'manual') AS source
        FROM tags t
        JOIN media_tags mt ON t.id = mt.tag_id
        LEFT JOIN media_tag_provenance mtp ON mt.media_id = mtp.media_id AND mt.tag_id = mtp.tag_id
        WHERE mt.media_id = ?
        ORDER BY t.name ASC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);

    std::vector<Tag> tags;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        Tag t;
        t.id = stmt.getInt64(0);
        t.name = stmt.getString(1);
        t.category = stmt.getString(2);
        if (!stmt.isNull(3)) {
            t.parent_id = stmt.getInt64(3);
        }
        t.source = stmt.getString(4);
        tags.push_back(std::move(t));
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read tags for media: " + conn_.lastErrorMessage());
    }
    return tags;
}

Result<std::unordered_map<MediaId, std::vector<Tag>>> CatalogDb::getTagsForMediaBatch(const std::vector<MediaId>& mediaIds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::unordered_map<MediaId, std::vector<Tag>> result;
    if (mediaIds.empty()) {
        return result;
    }

    constexpr size_t kBatchChunkSize = 500;
    for (size_t chunkStart = 0; chunkStart < mediaIds.size(); chunkStart += kBatchChunkSize) {
        size_t chunkEnd = std::min(chunkStart + kBatchChunkSize, mediaIds.size());
        size_t count = chunkEnd - chunkStart;

        std::string sql = R"SQL(
            SELECT mt.media_id, t.id, t.name, t.category, t.parent_id, COALESCE(mtp.source, 'manual') AS source
            FROM tags t
            JOIN media_tags mt ON t.id = mt.tag_id
            LEFT JOIN media_tag_provenance mtp ON mt.media_id = mtp.media_id AND mt.tag_id = mtp.tag_id
            WHERE mt.media_id IN (
        )SQL";

        for (size_t i = 0; i < count; ++i) {
            if (i > 0) sql += ",";
            sql += "?";
        }
        sql += ") ORDER BY t.name ASC;";

        auto stmtRes = conn_.prepare(sql);
        if (!stmtRes.isOk()) return stmtRes.status();
        auto stmt = std::move(stmtRes.value());

        for (size_t i = 0; i < count; ++i) {
            stmt.bind(static_cast<int>(i + 1), mediaIds[chunkStart + i]);
        }

        StepResult stepRes = StepResult::Done;
        while ((stepRes = stmt.step()) == StepResult::Row) {
            MediaId mid = stmt.getInt64(0);
            Tag t;
            t.id = stmt.getInt64(1);
            t.name = stmt.getString(2);
            t.category = stmt.getString(3);
            if (!stmt.isNull(4)) {
                t.parent_id = stmt.getInt64(4);
            }
            t.source = stmt.getString(5);
            result[mid].push_back(std::move(t));
        }
        if (stepRes == StepResult::Error) {
            return Status::databaseError("Failed to read batch tags: " + conn_.lastErrorMessage());
        }
    }
    return result;
}

Status CatalogDb::addTagToMedia(MediaId mediaId, TagId tagId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    auto stmtRes = conn_.prepare("INSERT OR IGNORE INTO media_tags (media_id, tag_id) VALUES (?, ?);");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, mediaId);
    stmt.bind(2, tagId);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to add tag to media: " + conn_.lastErrorMessage());
    }
    auto provenanceRes = conn_.prepare(
        "INSERT INTO media_tag_provenance(media_id,tag_id,manual,source) VALUES(?,?,1,'manual') "
        "ON CONFLICT(media_id,tag_id) DO UPDATE SET manual=1, source='manual';");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value());
    provenance.bind(1, mediaId); provenance.bind(2, tagId);
    if (provenance.step() != StepResult::Done) {
        return Status::databaseError("Failed to record manual tag assignment: " + conn_.lastErrorMessage());
    }

    auto rejRes = conn_.prepare("DELETE FROM smart_tag_rejections WHERE media_id=? AND tag_id=?;");
    if (!rejRes.isOk()) return rejRes.status();
    auto rejStmt = std::move(rejRes.value());
    rejStmt.bind(1, mediaId); rejStmt.bind(2, tagId);
    if (rejStmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to clear smart tag rejection: " + conn_.lastErrorMessage());
    }

    return tx.commit();
}

Status CatalogDb::removeTagFromMedia(MediaId mediaId, TagId tagId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    auto categoryRes = conn_.prepare("SELECT category FROM tags WHERE id=?;");
    if (!categoryRes.isOk()) return categoryRes.status();
    auto categoryStmt = std::move(categoryRes.value()); categoryStmt.bind(1, tagId);
    bool peopleTag = categoryStmt.step() == StepResult::Row && categoryStmt.getString(0) == "people";
    if (peopleTag) {
        auto rejectRes = conn_.prepare(R"SQL(
            INSERT OR IGNORE INTO face_rejections(face_id,tag_id,rejected_at)
            SELECT id,?,? FROM faces WHERE media_id=? AND person_tag_id=?;
        )SQL");
        if (!rejectRes.isOk()) return rejectRes.status();
        auto reject = std::move(rejectRes.value()); reject.bind(1, tagId); reject.bind(2, currentUnixTime());
        reject.bind(3, mediaId); reject.bind(4, tagId);
        if (reject.step() != StepResult::Done) return Status::databaseError("Failed to record removed people identities: " + conn_.lastErrorMessage());
        auto clearRes = conn_.prepare("UPDATE faces SET person_tag_id=NULL,revision=revision+1,updated_at=? WHERE media_id=? AND person_tag_id=?;");
        if (!clearRes.isOk()) return clearRes.status();
        auto clear = std::move(clearRes.value()); clear.bind(1, currentUnixTime()); clear.bind(2, mediaId); clear.bind(3, tagId);
        if (clear.step() != StepResult::Done) return Status::databaseError("Failed to clear removed people identities: " + conn_.lastErrorMessage());
    }
    auto provCheckRes = conn_.prepare("SELECT source FROM media_tag_provenance WHERE media_id=? AND tag_id=?;");
    if (!provCheckRes.isOk()) return provCheckRes.status();
    auto provCheck = std::move(provCheckRes.value());
    provCheck.bind(1, mediaId);
    provCheck.bind(2, tagId);
    if (provCheck.step() == StepResult::Row && !provCheck.isNull(0) && provCheck.getString(0) == "clip") {
        auto smartRejRes = conn_.prepare("INSERT OR REPLACE INTO smart_tag_rejections(media_id,tag_id,rejected_at) VALUES(?,?,?);");
        if (!smartRejRes.isOk()) return smartRejRes.status();
        auto smartRej = std::move(smartRejRes.value());
        smartRej.bind(1, mediaId);
        smartRej.bind(2, tagId);
        smartRej.bind(3, currentUnixTime());
        if (smartRej.step() != StepResult::Done) {
            return Status::databaseError("Failed to record smart tag rejection: " + conn_.lastErrorMessage());
        }
    }

    auto stmtRes = conn_.prepare("DELETE FROM media_tags WHERE media_id = ? AND tag_id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, mediaId);
    stmt.bind(2, tagId);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to remove tag from media: " + conn_.lastErrorMessage());
    }
    auto provenanceRes = conn_.prepare("DELETE FROM media_tag_provenance WHERE media_id=? AND tag_id=?;");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value()); provenance.bind(1, mediaId); provenance.bind(2, tagId);
    if (provenance.step() != StepResult::Done) return Status::databaseError("Failed to clear tag provenance: " + conn_.lastErrorMessage());
    return tx.commit();
}

static Status invalidateFaceAnalysis(Connection& conn, MediaId mediaId) {
    auto facesRes = conn.prepare("DELETE FROM faces WHERE media_id=?;");
    if (!facesRes.isOk()) return facesRes.status();
    auto faces = std::move(facesRes.value()); faces.bind(1, mediaId);
    if (faces.step() != StepResult::Done) return Status::databaseError("Failed to invalidate faces: " + conn.lastErrorMessage());

    auto analysisRes = conn.prepare("DELETE FROM face_media_analysis WHERE media_id=?;");
    if (!analysisRes.isOk()) return analysisRes.status();
    auto analysis = std::move(analysisRes.value()); analysis.bind(1, mediaId);
    if (analysis.step() != StepResult::Done) return Status::databaseError("Failed to invalidate face analysis: " + conn.lastErrorMessage());

    auto linkRes = conn.prepare(R"SQL(
        DELETE FROM media_tags
        WHERE media_id = ? AND tag_id IN (
            SELECT tag_id FROM media_tag_provenance WHERE media_id = ? AND manual = 0
        );
    )SQL");
    if (!linkRes.isOk()) return linkRes.status();
    auto link = std::move(linkRes.value());
    link.bind(1, mediaId); link.bind(2, mediaId);
    if (link.step() != StepResult::Done) return Status::databaseError("Failed to remove stale face people tag: " + conn.lastErrorMessage());

    auto provenanceRes = conn.prepare("DELETE FROM media_tag_provenance WHERE media_id = ? AND manual = 0;");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value());
    provenance.bind(1, mediaId);
    if (provenance.step() != StepResult::Done) return Status::databaseError("Failed to remove stale face tag ownership: " + conn.lastErrorMessage());

    auto semanticRes = conn.prepare("DELETE FROM semantic_index_state WHERE media_id = ?;");
    if (!semanticRes.isOk()) return semanticRes.status();
    auto semantic = std::move(semanticRes.value());
    semantic.bind(1, mediaId);
    if (semantic.step() != StepResult::Done) return Status::databaseError("Failed to invalidate semantic index state: " + conn.lastErrorMessage());

    auto smartStateRes = conn.prepare("DELETE FROM smart_tag_state WHERE media_id = ?;");
    if (!smartStateRes.isOk()) return smartStateRes.status();
    auto smartState = std::move(smartStateRes.value());
    smartState.bind(1, mediaId);
    if (smartState.step() != StepResult::Done) return Status::databaseError("Failed to invalidate smart tag state: " + conn.lastErrorMessage());

    auto smartRejRes = conn.prepare("DELETE FROM smart_tag_rejections WHERE media_id = ?;");
    if (!smartRejRes.isOk()) return smartRejRes.status();
    auto smartRej = std::move(smartRejRes.value());
    smartRej.bind(1, mediaId);
    if (smartRej.step() != StepResult::Done) return Status::databaseError("Failed to invalidate smart tag rejections: " + conn.lastErrorMessage());

    return Status::ok();
}

static Result<bool> mediaFaceGeometryChanged(Connection& conn, const MediaItem& item) {
    auto previousRes = conn.prepare("SELECT content_hash,width,height,orientation,media_type FROM media_items WHERE id=?;");
    if (!previousRes.isOk()) return previousRes.status();
    auto previous = std::move(previousRes.value()); previous.bind(1, item.id);
    if (previous.step() != StepResult::Row) return false;
    return previous.getString(0) != item.content_hash || previous.getInt(1) != item.width ||
           previous.getInt(2) != item.height || previous.getInt(3) != item.exif.orientation ||
           previous.getString(4) != item.media_type;
}

Result<TagId> CatalogDb::replaceTagAssignments(
    const std::string& name,
    const std::string& category,
    const std::vector<MediaId>& mediaIds
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto tagRes = createOrGetTag(name, category);
    if (!tagRes.isOk()) return tagRes.status();
    TagId tagId = tagRes.value();

    std::unordered_set<MediaId> uniqueMediaIds(mediaIds.begin(), mediaIds.end());

    std::unordered_set<MediaId> existingMediaIds;
    {
        auto existingRes = conn_.prepare("SELECT media_id FROM media_tags WHERE tag_id = ?;");
        if (!existingRes.isOk()) return existingRes.status();
        auto existingStmt = std::move(existingRes.value());
        existingStmt.bind(1, tagId);
        StepResult stepRes = StepResult::Done;
        while ((stepRes = existingStmt.step()) == StepResult::Row) {
            existingMediaIds.insert(existingStmt.getInt64(0));
        }
        if (stepRes == StepResult::Error) {
            return Status::databaseError("Failed to query existing tag assignments: " + conn_.lastErrorMessage());
        }
    }

    std::vector<MediaId> removedMediaIds;
    for (MediaId oldId : existingMediaIds) {
        if (!uniqueMediaIds.contains(oldId)) {
            removedMediaIds.push_back(oldId);
        }
    }

    if (!removedMediaIds.empty()) {
        const int64_t now = currentUnixTime();
        if (category == "people") {
            auto rejectRes = conn_.prepare(R"SQL(
                INSERT OR IGNORE INTO face_rejections(face_id,tag_id,rejected_at)
                SELECT id,?,? FROM faces WHERE media_id=? AND person_tag_id=?;
            )SQL");
            if (!rejectRes.isOk()) return rejectRes.status();
            auto reject = std::move(rejectRes.value());

            auto clearRes = conn_.prepare(
                "UPDATE faces SET person_tag_id=NULL,revision=revision+1,updated_at=? WHERE media_id=? AND person_tag_id=?;");
            if (!clearRes.isOk()) return clearRes.status();
            auto clear = std::move(clearRes.value());

            for (MediaId remId : removedMediaIds) {
                reject.reset();
                reject.bind(1, tagId);
                reject.bind(2, now);
                reject.bind(3, remId);
                reject.bind(4, tagId);
                if (reject.step() != StepResult::Done) {
                    return Status::databaseError("Failed to record removed people identities: " + conn_.lastErrorMessage());
                }

                clear.reset();
                clear.bind(1, now);
                clear.bind(2, remId);
                clear.bind(3, tagId);
                if (clear.step() != StepResult::Done) {
                    return Status::databaseError("Failed to clear removed people identities: " + conn_.lastErrorMessage());
                }
            }
        }

        auto deleteRes = conn_.prepare("DELETE FROM media_tags WHERE media_id = ? AND tag_id = ?;");
        if (!deleteRes.isOk()) return deleteRes.status();
        auto deleteStmt = std::move(deleteRes.value());

        auto clearProvenanceRes = conn_.prepare("DELETE FROM media_tag_provenance WHERE media_id = ? AND tag_id = ?;");
        if (!clearProvenanceRes.isOk()) return clearProvenanceRes.status();
        auto clearProvenance = std::move(clearProvenanceRes.value());

        for (MediaId remId : removedMediaIds) {
            deleteStmt.reset();
            deleteStmt.bind(1, remId);
            deleteStmt.bind(2, tagId);
            if (deleteStmt.step() != StepResult::Done) {
                return Status::databaseError("Failed to remove tag assignment: " + conn_.lastErrorMessage());
            }

            clearProvenance.reset();
            clearProvenance.bind(1, remId);
            clearProvenance.bind(2, tagId);
            if (clearProvenance.step() != StepResult::Done) {
                return Status::databaseError("Failed to clear tag assignment provenance: " + conn_.lastErrorMessage());
            }
        }
    }

    auto insertRes = conn_.prepare("INSERT OR IGNORE INTO media_tags (media_id, tag_id) VALUES (?, ?);");
    if (!insertRes.isOk()) return insertRes.status();
    auto insertStmt = std::move(insertRes.value());

    auto provenanceRes = conn_.prepare(
        "INSERT INTO media_tag_provenance(media_id,tag_id,manual,source) VALUES(?,?,1,'manual') "
        "ON CONFLICT(media_id,tag_id) DO UPDATE SET manual=1, source='manual';");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value());

    auto clearRejRes = conn_.prepare("DELETE FROM smart_tag_rejections WHERE media_id=? AND tag_id=?;");
    if (!clearRejRes.isOk()) return clearRejRes.status();
    auto clearRej = std::move(clearRejRes.value());

    for (MediaId mediaId : uniqueMediaIds) {
        clearRej.reset();
        clearRej.bind(1, mediaId);
        clearRej.bind(2, tagId);
        if (clearRej.step() != StepResult::Done) {
            return Status::databaseError("Failed to clear smart tag rejections: " + conn_.lastErrorMessage());
        }

        if (existingMediaIds.contains(mediaId)) {
            continue;
        }

        insertStmt.reset();
        insertStmt.bind(1, mediaId);
        insertStmt.bind(2, tagId);
        if (insertStmt.step() != StepResult::Done) {
            return Status::databaseError("Failed to replace tag assignments: " + conn_.lastErrorMessage());
        }

        provenance.reset();
        provenance.bind(1, mediaId);
        provenance.bind(2, tagId);
        if (provenance.step() != StepResult::Done) {
            return Status::databaseError("Failed to save tag assignment provenance: " + conn_.lastErrorMessage());
        }
    }

    Status commitStatus = tx.commit();
    if (!commitStatus.isOk()) return commitStatus;
    return tagId;
}

Status CatalogDb::deleteTag(TagId tagId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    auto clearFacesRes = conn_.prepare("UPDATE faces SET person_tag_id=NULL,revision=revision+1,updated_at=? WHERE person_tag_id=?;");
    if (!clearFacesRes.isOk()) return clearFacesRes.status();
    auto clearFaces = std::move(clearFacesRes.value()); clearFaces.bind(1, currentUnixTime()); clearFaces.bind(2, tagId);
    if (clearFaces.step() != StepResult::Done) return Status::databaseError("Failed to clear face identities for deleted tag: " + conn_.lastErrorMessage());

    auto stmtRes = conn_.prepare("DELETE FROM tags WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, tagId);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to delete tag: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Tag not found: " + std::to_string(tagId));
    }
    return tx.commit();
}

Status CatalogDb::replaceSmartTags(
    const std::vector<SmartTagRecord>& records,
    const std::string& modelId,
    const std::string& modelRevision,
    const std::string& taxonomyVersion
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_.isOpen()) {
        return Status::internal("Database not open");
    }

    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto delMediaTagsRes = conn_.prepare(R"SQL(
        DELETE FROM media_tags
        WHERE media_id = ? AND tag_id IN (
            SELECT tag_id FROM media_tag_provenance WHERE media_id = ? AND source = 'clip'
        );
    )SQL");
    if (!delMediaTagsRes.isOk()) return delMediaTagsRes.status();
    auto delMediaTags = std::move(delMediaTagsRes.value());

    auto delProvRes = conn_.prepare("DELETE FROM media_tag_provenance WHERE media_id = ? AND source = 'clip';");
    if (!delProvRes.isOk()) return delProvRes.status();
    auto delProv = std::move(delProvRes.value());

    auto insMediaTagRes = conn_.prepare("INSERT OR IGNORE INTO media_tags(media_id, tag_id) VALUES(?, ?);");
    if (!insMediaTagRes.isOk()) return insMediaTagRes.status();
    auto insMediaTag = std::move(insMediaTagRes.value());

    auto insProvRes = conn_.prepare(
        "INSERT INTO media_tag_provenance(media_id, tag_id, manual, source) VALUES(?, ?, 0, 'clip') "
        "ON CONFLICT(media_id, tag_id) DO NOTHING;");
    if (!insProvRes.isOk()) return insProvRes.status();
    auto insProv = std::move(insProvRes.value());

    auto upsertStateRes = conn_.prepare(R"SQL(
        INSERT INTO smart_tag_state(media_id, content_hash, model_id, model_revision, taxonomy_version, label_count, updated_at)
        VALUES(?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(media_id) DO UPDATE SET
            content_hash = excluded.content_hash,
            model_id = excluded.model_id,
            model_revision = excluded.model_revision,
            taxonomy_version = excluded.taxonomy_version,
            label_count = excluded.label_count,
            updated_at = excluded.updated_at;
    )SQL");
    if (!upsertStateRes.isOk()) return upsertStateRes.status();
    auto upsertState = std::move(upsertStateRes.value());

    int64_t now = currentUnixTime();

    for (const auto& rec : records) {
        delMediaTags.reset();
        delMediaTags.bind(1, rec.mediaId);
        delMediaTags.bind(2, rec.mediaId);
        if (delMediaTags.step() != StepResult::Done) {
            return Status::databaseError("Failed to delete old clip media tags: " + conn_.lastErrorMessage());
        }

        delProv.reset();
        delProv.bind(1, rec.mediaId);
        if (delProv.step() != StepResult::Done) {
            return Status::databaseError("Failed to delete old clip tag provenance: " + conn_.lastErrorMessage());
        }

        for (TagId tagId : rec.tagIds) {
            insMediaTag.reset();
            insMediaTag.bind(1, rec.mediaId);
            insMediaTag.bind(2, tagId);
            if (insMediaTag.step() != StepResult::Done) {
                return Status::databaseError("Failed to insert smart media tag: " + conn_.lastErrorMessage());
            }

            insProv.reset();
            insProv.bind(1, rec.mediaId);
            insProv.bind(2, tagId);
            if (insProv.step() != StepResult::Done) {
                return Status::databaseError("Failed to insert smart tag provenance: " + conn_.lastErrorMessage());
            }
        }

        upsertState.reset();
        upsertState.bind(1, rec.mediaId);
        upsertState.bind(2, rec.contentHash);
        upsertState.bind(3, modelId);
        upsertState.bind(4, modelRevision);
        upsertState.bind(5, taxonomyVersion);
        upsertState.bind(6, static_cast<int64_t>(rec.tagIds.size()));
        upsertState.bind(7, now);
        if (upsertState.step() != StepResult::Done) {
            return Status::databaseError("Failed to upsert smart tag state: " + conn_.lastErrorMessage());
        }
    }

    return tx.commit();
}

Status CatalogDb::clearSmartTags() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_.isOpen()) {
        return Status::internal("Database not open");
    }

    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    Status s = conn_.execute(R"SQL(
        DELETE FROM media_tags
        WHERE (media_id, tag_id) IN (
            SELECT media_id, tag_id FROM media_tag_provenance WHERE source = 'clip'
        );
    )SQL");
    if (!s.isOk()) return s;

    s = conn_.execute("DELETE FROM media_tag_provenance WHERE source = 'clip';");
    if (!s.isOk()) return s;

    s = conn_.execute("DELETE FROM smart_tag_state;");
    if (!s.isOk()) return s;

    s = conn_.execute("DELETE FROM smart_tag_rejections;");
    if (!s.isOk()) return s;

    return tx.commit();
}

// --- Albums ---

Result<AlbumId> CatalogDb::createAlbum(const std::string& name, const std::string& description, bool is_smart, const std::string& query_json) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("INSERT INTO albums (name, description, is_smart, query_json, created_at) VALUES (?, ?, ?, ?, ?);");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, name);
    stmt.bind(2, description);
    stmt.bind(3, is_smart ? 1 : 0);
    stmt.bind(4, query_json);
    stmt.bind(5, currentUnixTime());

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to create album: " + conn_.lastErrorMessage());
    }
    return conn_.lastInsertRowId();
}

Result<std::vector<Album>> CatalogDb::getAllAlbums() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        SELECT a.id, a.name, a.description, a.is_smart, a.query_json, a.cover_media_id,
               a.created_at, COUNT(am.media_id) AS item_count
        FROM albums a
        LEFT JOIN album_media am ON a.id = am.album_id
        GROUP BY a.id
        ORDER BY a.name ASC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::vector<Album> albums;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        Album a;
        a.id = stmt.getInt64(0);
        a.name = stmt.getString(1);
        a.description = stmt.getString(2);
        a.is_smart = (stmt.getInt(3) != 0);
        a.query_json = stmt.getString(4);
        if (!stmt.isNull(5)) {
            a.cover_media_id = stmt.getInt64(5);
        }
        a.created_at = stmt.getInt64(6);
        a.item_count = stmt.getInt64(7);
        albums.push_back(std::move(a));
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read albums: " + conn_.lastErrorMessage());
    }
    return albums;
}

Result<Album> CatalogDb::getAlbumById(AlbumId id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        SELECT a.id, a.name, a.description, a.is_smart, a.query_json, a.cover_media_id,
               a.created_at, COUNT(am.media_id) AS item_count
        FROM albums a
        LEFT JOIN album_media am ON a.id = am.album_id
        WHERE a.id = ?
        GROUP BY a.id;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);

    if (stmt.step() == StepResult::Row) {
        Album a;
        a.id = stmt.getInt64(0);
        a.name = stmt.getString(1);
        a.description = stmt.getString(2);
        a.is_smart = (stmt.getInt(3) != 0);
        a.query_json = stmt.getString(4);
        if (!stmt.isNull(5)) {
            a.cover_media_id = stmt.getInt64(5);
        }
        a.created_at = stmt.getInt64(6);
        a.item_count = stmt.getInt64(7);
        return a;
    }
    return Status::notFound("Album not found with id " + std::to_string(id));
}

Status CatalogDb::deleteAlbum(AlbumId id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    auto s1 = conn_.prepare("DELETE FROM album_media WHERE album_id = ?;");
    if (!s1.isOk()) return s1.status();
    auto stmt1 = std::move(s1.value());
    stmt1.bind(1, id);
    if (stmt1.step() != StepResult::Done) {
        return Status::databaseError("Failed to delete album media: " + conn_.lastErrorMessage());
    }

    auto stmtRes = conn_.prepare("DELETE FROM albums WHERE id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to delete album: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Album not found: " + std::to_string(id));
    }
    return tx.commit();
}

Status CatalogDb::addMediaToAlbum(AlbumId albumId, MediaId mediaId, int position) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto stmtRes = conn_.prepare("INSERT OR REPLACE INTO album_media (album_id, media_id, position) VALUES (?, ?, ?);");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, albumId);
    stmt.bind(2, mediaId);
    stmt.bind(3, position);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to add media to album: " + conn_.lastErrorMessage());
    }
    return Status::ok();
}

Status CatalogDb::reorderAlbumMedia(AlbumId albumId, const std::vector<MediaId>& mediaIds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto albumStmtRes = conn_.prepare("SELECT 1 FROM albums WHERE id = ?;");
    if (!albumStmtRes.isOk()) return albumStmtRes.status();
    auto albumStmt = std::move(albumStmtRes.value());
    albumStmt.bind(1, albumId);
    if (albumStmt.step() != StepResult::Row) {
        return Status::notFound("Album not found: " + std::to_string(albumId));
    }

    std::unordered_set<MediaId> requestedIds;
    requestedIds.reserve(mediaIds.size());
    for (MediaId mediaId : mediaIds) {
        if (mediaId <= 0 || !requestedIds.insert(mediaId).second) {
            return Status::invalidArgument("Album order must contain unique positive media IDs");
        }
    }

    auto membersStmtRes = conn_.prepare("SELECT media_id FROM album_media WHERE album_id = ?;");
    if (!membersStmtRes.isOk()) return membersStmtRes.status();
    auto membersStmt = std::move(membersStmtRes.value());
    membersStmt.bind(1, albumId);
    std::unordered_set<MediaId> memberIds;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = membersStmt.step()) == StepResult::Row) {
        memberIds.insert(membersStmt.getInt64(0));
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read album media members: " + conn_.lastErrorMessage());
    }
    if (requestedIds != memberIds) {
        return Status::invalidArgument("Album order must contain every album media ID exactly once");
    }

    auto updateStmtRes = conn_.prepare(
        "UPDATE album_media SET position = ? WHERE album_id = ? AND media_id = ?;");
    if (!updateStmtRes.isOk()) return updateStmtRes.status();
    auto updateStmt = std::move(updateStmtRes.value());
    for (size_t position = 0; position < mediaIds.size(); ++position) {
        updateStmt.bind(1, static_cast<int64_t>(position));
        updateStmt.bind(2, albumId);
        updateStmt.bind(3, mediaIds[position]);
        if (updateStmt.step() != StepResult::Done) {
            return Status::databaseError("Failed to update album order: " + conn_.lastErrorMessage());
        }
        updateStmt.reset();
    }

    return tx.commit();
}

Status CatalogDb::setAlbumCover(AlbumId albumId, MediaId mediaId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        UPDATE albums
        SET cover_media_id = ?
        WHERE id = ?
          AND EXISTS (
              SELECT 1
              FROM album_media AS am
              JOIN media_items AS mi ON mi.id = am.media_id
              WHERE am.album_id = ?
                AND am.media_id = ?
                AND mi.media_type = 'photo'
          );
    )SQL";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, mediaId);
    stmt.bind(2, albumId);
    stmt.bind(3, albumId);
    stmt.bind(4, mediaId);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to set album cover: " + conn_.lastErrorMessage());
    }
    if (conn_.changes() == 0) {
        return Status::notFound("Album or media item is not found in the album");
    }
    return Status::ok();
}

Status CatalogDb::removeMediaFromAlbum(AlbumId albumId, MediaId mediaId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto clearCoverRes = conn_.prepare(
        "UPDATE albums SET cover_media_id = NULL WHERE id = ? AND cover_media_id = ?;");
    if (!clearCoverRes.isOk()) return clearCoverRes.status();
    auto clearCover = std::move(clearCoverRes.value());
    clearCover.bind(1, albumId);
    clearCover.bind(2, mediaId);
    if (clearCover.step() != StepResult::Done) {
        return Status::databaseError("Failed to clear album cover: " + conn_.lastErrorMessage());
    }

    auto stmtRes = conn_.prepare("DELETE FROM album_media WHERE album_id = ? AND media_id = ?;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, albumId);
    stmt.bind(2, mediaId);
    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to remove media from album: " + conn_.lastErrorMessage());
    }

    return tx.commit();
}

Result<std::vector<MediaItem>> CatalogDb::getMediaInAlbum(AlbumId albumId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kMediaColumnsProjection +
                      " FROM media_items m JOIN album_media am ON m.id = am.media_id WHERE am.album_id = ? ORDER BY am.position ASC, m.date_taken DESC;";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, albumId);

    std::vector<MediaItem> items;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        items.push_back(extractMediaItem(stmt));
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read media in album: " + conn_.lastErrorMessage());
    }
    return items;
}

// --- Timeline & Stats ---

Result<std::vector<TimelineEntry>> CatalogDb::getTimeline() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Group by strftime year and month from unix timestamp date_taken
    const char* sql = R"SQL(
        SELECT CAST(strftime('%Y', date_taken, 'unixepoch') AS INTEGER) AS y,
               CAST(strftime('%m', date_taken, 'unixepoch') AS INTEGER) AS m,
               COUNT(*) AS cnt
        FROM media_items
        WHERE date_taken != 0
        GROUP BY y, m
        ORDER BY y DESC, m DESC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::vector<TimelineEntry> timeline;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        TimelineEntry entry;
        entry.year = stmt.getInt(0);
        entry.month = stmt.getInt(1);
        entry.count = stmt.getInt64(2);
        timeline.push_back(entry);
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read timeline: " + conn_.lastErrorMessage());
    }
    return timeline;
}

Result<CatalogStats> CatalogDb::getStats() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    CatalogStats s;

    const char* mediaSql = R"SQL(
        SELECT
            COUNT(*),
            COALESCE(SUM(file_size), 0),
            COALESCE(MIN(CASE WHEN date_taken != 0 THEN date_taken END), 0),
            COALESCE(MAX(CASE WHEN date_taken != 0 THEN date_taken END), 0),
            COUNT(CASE WHEN media_type = 'video' THEN 1 END),
            COUNT(CASE WHEN media_type = 'audio' THEN 1 END),
            COUNT(CASE WHEN media_type NOT IN ('video', 'audio') THEN 1 END),
            COALESCE(SUM(CASE WHEN media_type IN ('video', 'audio') THEN duration ELSE 0 END), 0.0),
            COUNT(CASE WHEN flag = 1 THEN 1 END),
            COUNT(CASE WHEN flag = -1 THEN 1 END),
            COUNT(CASE WHEN rating = 0 THEN 1 END)
        FROM media_items;
    )SQL";

    auto stmtRes = conn_.prepare(mediaSql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    if (stmt.step() == StepResult::Row) {
        s.total_media = stmt.getInt64(0);
        s.total_size_bytes = stmt.getInt64(1);
        s.earliest_date = stmt.getInt64(2);
        s.latest_date = stmt.getInt64(3);
        s.total_videos = stmt.getInt64(4);
        s.total_audio = stmt.getInt64(5);
        s.total_photos = stmt.getInt64(6);
        s.total_duration = stmt.getDouble(7);
        s.total_picks = stmt.getInt64(8);
        s.total_rejects = stmt.getInt64(9);
        s.total_not_rejects = s.total_media - s.total_rejects;
        s.total_unrated = stmt.getInt64(10);
    }

    // tags count
    {
        auto tagStmtRes = conn_.prepare("SELECT COUNT(*) FROM tags;");
        if (!tagStmtRes.isOk()) return tagStmtRes.status();
        auto tagStmt = std::move(tagStmtRes.value());
        if (tagStmt.step() == StepResult::Row) {
            s.total_tags = tagStmt.getInt64(0);
        }
    }

    // albums count
    {
        auto albStmtRes = conn_.prepare("SELECT COUNT(*) FROM albums;");
        if (!albStmtRes.isOk()) return albStmtRes.status();
        auto albStmt = std::move(albStmtRes.value());
        if (albStmt.step() == StepResult::Row) {
            s.total_albums = albStmt.getInt64(0);
        }
    }

    // exact duplicate groups count
    {
        const char* dupSql = R"SQL(
            SELECT COUNT(*)
            FROM (
                SELECT 1
                FROM media_items
                WHERE content_hash != ""
                GROUP BY content_hash, file_size
                HAVING COUNT(*) > 1
            );
        )SQL";
        auto dupStmtRes = conn_.prepare(dupSql);
        if (dupStmtRes.isOk()) {
            auto dupStmt = std::move(dupStmtRes.value());
            if (dupStmt.step() == StepResult::Row) {
                s.total_duplicates = dupStmt.getInt64(0);
            }
        }
    }

    return s;
}

Result<std::vector<std::string>> CatalogDb::getAllFolders() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = "SELECT file_path FROM media_items;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::set<std::string> folders;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        std::string path = stmt.getString(0);
        std::replace(path.begin(), path.end(), '\\', '/');
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != std::string::npos && lastSlash > 0) {
            folders.insert(path.substr(0, lastSlash));
        }
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read folders: " + conn_.lastErrorMessage());
    }
    return std::vector<std::string>(folders.begin(), folders.end());
}

Result<std::vector<MediaItem>> CatalogDb::queryMedia(
    const std::string& whereClause,
    const std::vector<std::string>& params,
    const std::string& orderBy,
    int limit,
    int offset
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kMediaColumnsProjection + " FROM media_items";
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    if (!orderBy.empty()) {
        sql += " ORDER BY " + orderBy;
    } else {
        sql += " ORDER BY date_taken DESC, id DESC";
    }
    if (limit > 0) {
        sql += " LIMIT " + std::to_string(limit);
        if (offset > 0) {
            sql += " OFFSET " + std::to_string(offset);
        }
    }

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    for (size_t i = 0; i < params.size(); ++i) {
        Status bs = stmt.bind(static_cast<int>(i + 1), params[i]);
        if (!bs.isOk()) return bs;
    }

    std::vector<MediaItem> items;
    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        items.push_back(extractMediaItem(stmt));
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to query media: " + conn_.lastErrorMessage());
    }
    return items;
}

Result<int64_t> CatalogDb::countMedia(
    const std::string& whereClause,
    const std::vector<std::string>& params
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql = "SELECT COUNT(*) FROM media_items";
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    for (size_t i = 0; i < params.size(); ++i) {
        Status bs = stmt.bind(static_cast<int>(i + 1), params[i]);
        if (!bs.isOk()) return bs;
    }

    StepResult stepRes = stmt.step();
    if (stepRes == StepResult::Row) {
        return stmt.getInt64(0);
    } else if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to count media: " + conn_.lastErrorMessage());
    }
    return 0;
}

Result<std::vector<std::string>> CatalogDb::getMediaHashesByType(const std::string& mediaType) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string sql = "SELECT content_hash FROM media_items WHERE media_type = ? AND content_hash != '';";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) {
        return Result<std::vector<std::string>>::failure(stmtRes.status());
    }
    auto& stmt = stmtRes.value();
    stmt.bind(1, mediaType);
    std::vector<std::string> hashes;
    StepResult stepRes;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        hashes.push_back(stmt.getString(0));
    }
    return hashes;
}

Result<std::vector<std::string>> CatalogDb::getVideoHashes() {
    return getMediaHashesByType("video");
}

Result<int64_t> CatalogDb::makePathsRelative(const std::string& photosDir, const std::string& thumbsDir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_.isOpen()) {
        return Status::internal("Database not open");
    }

    std::filesystem::path normPhotos;
    if (!photosDir.empty()) {
        auto photosFs = pathFromUtf8(photosDir);
        normPhotos = photosFs.lexically_normal();
    }

    std::filesystem::path normThumbs;
    if (!thumbsDir.empty()) {
        auto thumbsFs = pathFromUtf8(thumbsDir);
        normThumbs = thumbsFs.lexically_normal();
    }

    auto stmtRes = conn_.prepare("SELECT id, file_path, thumb_small, thumb_large FROM media_items;");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    struct RowUpdate {
        int64_t id;
        std::string file_path;
        std::string thumb_small;
        std::string thumb_large;
    };
    std::vector<RowUpdate> updates;
    std::unordered_set<std::string> knownFilePaths;

    auto makeRelativePath = [&](const std::string& origPath, const std::filesystem::path& normBase) -> std::pair<std::string, bool> {
        if (origPath.empty()) {
            return {"", false};
        }
        std::filesystem::path p = pathFromUtf8(origPath);
        std::string pathStr = origPath;
        bool changed = false;

        bool isAbs = p.is_absolute() || p.has_root_directory();
        if (!normBase.empty() && isAbs) {
            auto normalP = p.lexically_normal();
            auto rel = normalP.lexically_relative(normBase);
            bool validRel = !rel.empty();
            if (validRel) {
                std::string relStr = pathToUtf8(rel);
                std::replace(relStr.begin(), relStr.end(), '\\', '/');
                if (relStr == ".." || relStr.rfind("../", 0) == 0 || relStr == ".") {
                    validRel = false;
                }
            }
            if (!validRel) {
                std::error_code ec;
                auto canonP = std::filesystem::weakly_canonical(normalP, ec);
                if (!ec) {
                    auto canonBase = std::filesystem::weakly_canonical(normBase, ec);
                    if (!ec) {
                        rel = canonP.lexically_relative(canonBase);
                        validRel = !rel.empty();
                    }
                }
            }
            if (validRel) {
                std::string relStr = pathToUtf8(rel);
                std::replace(relStr.begin(), relStr.end(), '\\', '/');
                if (relStr != ".." && relStr.rfind("../", 0) != 0 && relStr != ".") {
                    if (relStr != pathStr) {
                        pathStr = relStr;
                        changed = true;
                    }
                }
            }
        }
        std::string genericPath = pathStr;
        std::replace(genericPath.begin(), genericPath.end(), '\\', '/');
        if (genericPath != pathStr) {
            pathStr = genericPath;
            changed = true;
        }
        return {pathStr, changed};
    };

    StepResult stepRes = StepResult::Done;
    while ((stepRes = stmt.step()) == StepResult::Row) {
        int64_t id = stmt.getInt64(0);
        std::string filePath = stmt.getString(1);
        std::string thumbSmall = stmt.getString(2);
        std::string thumbLarge = stmt.getString(3);

        auto [newFilePath, fpChanged] = makeRelativePath(filePath, normPhotos);
        auto [newThumbSmall, tsChanged] = makeRelativePath(thumbSmall, normThumbs);
        auto [newThumbLarge, tlChanged] = makeRelativePath(thumbLarge, normThumbs);

        if (fpChanged && !knownFilePaths.insert(newFilePath).second) {
            newFilePath = filePath;
            fpChanged = false;
        } else {
            knownFilePaths.insert(newFilePath);
        }

        if (fpChanged || tsChanged || tlChanged) {
            updates.push_back({id, newFilePath, newThumbSmall, newThumbLarge});
        }
    }
    if (stepRes == StepResult::Error) {
        return Status::databaseError("Failed to read media paths: " + conn_.lastErrorMessage());
    }

    if (updates.empty()) {
        return 0;
    }

    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    auto updateStmtRes = conn_.prepare("UPDATE OR IGNORE media_items SET file_path = ?, thumb_small = ?, thumb_large = ?, updated_at = ? WHERE id = ?;");
    if (!updateStmtRes.isOk()) return updateStmtRes.status();
    auto updateStmt = std::move(updateStmtRes.value());

    int64_t now = currentUnixTime();
    for (const auto& u : updates) {
        updateStmt.reset();
        updateStmt.bind(1, u.file_path);
        updateStmt.bind(2, u.thumb_small);
        updateStmt.bind(3, u.thumb_large);
        updateStmt.bind(4, now);
        updateStmt.bind(5, u.id);
        if (updateStmt.step() != StepResult::Done) {
            tx.rollback();
            return Status::internal("Failed to update media item path during relative migration: " + conn_.lastErrorMessage());
        }
    }

    Status commitStatus = tx.commit();
    if (!commitStatus.isOk()) return commitStatus;

    return static_cast<int64_t>(updates.size());
}

Result<std::vector<std::vector<MediaId>>> CatalogDb::findExactDuplicateGroups(const std::vector<MediaId>& mediaIds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string sql;
    std::vector<MediaId> uniqueIds;

    if (mediaIds.empty()) {
        sql = R"SQL(
            SELECT m.id, m.content_hash, m.file_size
            FROM media_items m
            JOIN (
                SELECT content_hash, file_size, COUNT(*) AS cnt
                FROM media_items
                WHERE content_hash != ''
                GROUP BY content_hash, file_size
                HAVING COUNT(*) > 1
            ) g ON m.content_hash = g.content_hash AND m.file_size = g.file_size
            WHERE m.content_hash != ''
            ORDER BY g.cnt DESC, g.file_size DESC, m.content_hash ASC, m.id ASC;
        )SQL";
    } else {
        std::unordered_set<MediaId> idSet(mediaIds.begin(), mediaIds.end());
        uniqueIds.assign(idSet.begin(), idSet.end());
        if (uniqueIds.size() < 2) {
            return std::vector<std::vector<MediaId>>{};
        }

        std::string placeholders;
        for (size_t i = 0; i < uniqueIds.size(); ++i) {
            if (i > 0) placeholders += ",";
            placeholders += "?";
        }

        sql = "SELECT m.id, m.content_hash, m.file_size "
              "FROM media_items m "
              "JOIN ("
              "  SELECT content_hash, file_size, COUNT(*) AS cnt "
              "  FROM media_items "
              "  WHERE content_hash != '' AND id IN (" + placeholders + ") "
              "  GROUP BY content_hash, file_size "
              "  HAVING COUNT(*) > 1 "
              ") g ON m.content_hash = g.content_hash AND m.file_size = g.file_size "
              "WHERE m.id IN (" + placeholders + ") "
              "ORDER BY g.cnt DESC, g.file_size DESC, m.content_hash ASC, m.id ASC;";
    }

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    if (!uniqueIds.empty()) {
        int idx = 1;
        for (MediaId id : uniqueIds) {
            stmt.bind(idx++, id);
        }
        for (MediaId id : uniqueIds) {
            stmt.bind(idx++, id);
        }
    }

    std::vector<std::vector<MediaId>> groups;
    std::string currentHash;
    int64_t currentSize = -1;
    std::vector<MediaId> currentGroup;

    StepResult sr;
    while ((sr = stmt.step()) == StepResult::Row) {
        MediaId id = stmt.getInt64(0);
        std::string hash = stmt.getString(1);
        int64_t size = stmt.getInt64(2);

        if (hash != currentHash || size != currentSize) {
            if (!currentGroup.empty()) {
                groups.push_back(std::move(currentGroup));
                currentGroup.clear();
            }
            currentHash = hash;
            currentSize = size;
        }
        currentGroup.push_back(id);
    }

    if (sr == StepResult::Error) {
        return Status::databaseError("Failed to find exact duplicate groups: " + conn_.lastErrorMessage());
    }

    if (!currentGroup.empty()) {
        groups.push_back(std::move(currentGroup));
    }

    return groups;
}

Status CatalogDb::dismissDuplicatePair(MediaId a, MediaId b) {
    if (a == b) {
        return Status::invalidArgument("Cannot dismiss duplicate pair with identical IDs");
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    MediaId idA = std::min(a, b);
    MediaId idB = std::max(a, b);
    int64_t now = currentUnixTime();

    const char* sql = "INSERT OR IGNORE INTO duplicate_dismissals (media_id_a, media_id_b, dismissed_at) VALUES (?, ?, ?);";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, idA);
    stmt.bind(2, idB);
    stmt.bind(3, now);
    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to dismiss duplicate pair: " + conn_.lastErrorMessage());
    }
    return Status::ok();
}

Result<std::vector<std::pair<MediaId, MediaId>>> CatalogDb::getDismissedDuplicatePairs() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = "SELECT media_id_a, media_id_b FROM duplicate_dismissals ORDER BY media_id_a ASC, media_id_b ASC;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::vector<std::pair<MediaId, MediaId>> result;
    StepResult sr;
    while ((sr = stmt.step()) == StepResult::Row) {
        result.emplace_back(stmt.getInt64(0), stmt.getInt64(1));
    }
    if (sr == StepResult::Error) {
        return Status::databaseError("Failed to read dismissed duplicate pairs: " + conn_.lastErrorMessage());
    }
    return result;
}

Result<int> CatalogDb::clearDismissedDuplicatePairs() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = "DELETE FROM duplicate_dismissals;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to clear dismissed duplicate pairs: " + conn_.lastErrorMessage());
    }
    return conn_.changes();
}

Status CatalogDb::mergeMediaMetadata(MediaId keeperId, MediaId sourceId) {
    if (keeperId == sourceId) {
        return Status::ok();
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }

    auto keeperRes = getMediaById(keeperId);
    if (!keeperRes.isOk()) return keeperRes.status();
    auto sourceRes = getMediaById(sourceId);
    if (!sourceRes.isOk()) return sourceRes.status();

    MediaItem keeper = std::move(keeperRes.value());
    const MediaItem& source = sourceRes.value();

    bool keeperUpdated = false;

    // 1. Inherit rating only when target empty (0)
    if (keeper.rating == 0 && source.rating != 0) {
        keeper.rating = source.rating;
        keeperUpdated = true;
    }

    // 2. Inherit flag only when target empty (Unflagged)
    if (keeper.flag == FlagState::Unflagged && source.flag != FlagState::Unflagged) {
        keeper.flag = source.flag;
        keeperUpdated = true;
    }

    // 3. Inherit date_taken only when target empty (keeper.date_taken == 0).
    // Invariant: Never overwrite keeper when keeper.date_taken != 0.
    // Negative pre-1970 timestamps are valid non-zero timestamps and are preserved, never overwritten.
    if (keeper.date_taken == 0 && source.date_taken != 0) {
        keeper.date_taken = source.date_taken;
        keeper.exif.date_taken_str = source.exif.date_taken_str;
        keeperUpdated = true;
    }

    // 4. Inherit GPS only when target empty (has_gps == false)
    if (!keeper.exif.has_gps && source.exif.has_gps) {
        keeper.exif.has_gps = true;
        keeper.exif.latitude = source.exif.latitude;
        keeper.exif.longitude = source.exif.longitude;
        keeper.exif.altitude = source.exif.altitude;
        keeperUpdated = true;
    }

    // 5. Inherit caption / title / description if target empty
    if (keeper.caption.empty() && !source.caption.empty()) {
        keeper.caption = source.caption;
        keeperUpdated = true;
    }
    if (keeper.audio_title.empty() && !source.audio_title.empty()) {
        keeper.audio_title = source.audio_title;
        keeperUpdated = true;
    }
    if (keeper.audio_artist.empty() && !source.audio_artist.empty()) {
        keeper.audio_artist = source.audio_artist;
        keeperUpdated = true;
    }
    if (keeper.audio_album.empty() && !source.audio_album.empty()) {
        keeper.audio_album = source.audio_album;
        keeperUpdated = true;
    }
    if (keeper.audio_genre.empty() && !source.audio_genre.empty()) {
        keeper.audio_genre = source.audio_genre;
        keeperUpdated = true;
    }
    if (keeper.exif.camera_make.empty() && !source.exif.camera_make.empty()) {
        keeper.exif.camera_make = source.exif.camera_make;
        keeperUpdated = true;
    }
    if (keeper.exif.camera_model.empty() && !source.exif.camera_model.empty()) {
        keeper.exif.camera_model = source.exif.camera_model;
        keeperUpdated = true;
    }
    if (keeper.exif.lens.empty() && !source.exif.lens.empty()) {
        keeper.exif.lens = source.exif.lens;
        keeperUpdated = true;
    }

    if (keeperUpdated) {
        Status s = updateMedia(keeper);
        if (!s.isOk()) return s;
    }

    // 6. Union tags and provenance (following getTagsForMediaBatch pattern)
    auto tagsBatchRes = getTagsForMediaBatch({keeperId, sourceId});
    if (!tagsBatchRes.isOk()) return tagsBatchRes.status();

    {
        auto tagInsertRes = conn_.prepare(
            "INSERT OR IGNORE INTO media_tags (media_id, tag_id) "
            "SELECT ?, tag_id FROM media_tags WHERE media_id = ?;");
        if (!tagInsertRes.isOk()) return tagInsertRes.status();
        auto tagInsert = std::move(tagInsertRes.value());
        tagInsert.bind(1, keeperId);
        tagInsert.bind(2, sourceId);
        if (tagInsert.step() != StepResult::Done) {
            return Status::databaseError("Failed to union tags: " + conn_.lastErrorMessage());
        }
    }

    {
        auto provRes = conn_.prepare(
            "INSERT INTO media_tag_provenance (media_id, tag_id, manual) "
            "SELECT ?, tag_id, manual FROM media_tag_provenance WHERE media_id = ? "
            "ON CONFLICT(media_id, tag_id) DO UPDATE SET manual = MAX(media_tag_provenance.manual, excluded.manual);");
        if (!provRes.isOk()) return provRes.status();
        auto prov = std::move(provRes.value());
        prov.bind(1, keeperId);
        prov.bind(2, sourceId);
        if (prov.step() != StepResult::Done) {
            return Status::databaseError("Failed to union tag provenance: " + conn_.lastErrorMessage());
        }
    }

    // 7. Album memberships preserving position (update-in-place if keeper absent else delete+compact)
    {
        auto albStmtRes = conn_.prepare("SELECT album_id, position FROM album_media WHERE media_id = ? ORDER BY album_id ASC;");
        if (!albStmtRes.isOk()) return albStmtRes.status();
        auto albStmt = std::move(albStmtRes.value());
        albStmt.bind(1, sourceId);

        struct AlbumRef {
            AlbumId albumId;
            int64_t position;
        };
        std::vector<AlbumRef> sourceAlbums;
        StepResult sr;
        while ((sr = albStmt.step()) == StepResult::Row) {
            sourceAlbums.push_back({albStmt.getInt64(0), albStmt.getInt64(1)});
        }
        if (sr == StepResult::Error) {
            return Status::databaseError("Failed to query source album memberships: " + conn_.lastErrorMessage());
        }

        for (const auto& ar : sourceAlbums) {
            auto checkKeeperRes = conn_.prepare("SELECT 1 FROM album_media WHERE album_id = ? AND media_id = ?;");
            if (!checkKeeperRes.isOk()) return checkKeeperRes.status();
            auto checkKeeper = std::move(checkKeeperRes.value());
            checkKeeper.bind(1, ar.albumId);
            checkKeeper.bind(2, keeperId);
            bool keeperPresent = (checkKeeper.step() == StepResult::Row);

            if (!keeperPresent) {
                auto updatePlaceRes = conn_.prepare(
                    "UPDATE album_media SET media_id = ? WHERE album_id = ? AND media_id = ?;");
                if (!updatePlaceRes.isOk()) return updatePlaceRes.status();
                auto updatePlace = std::move(updatePlaceRes.value());
                updatePlace.bind(1, keeperId);
                updatePlace.bind(2, ar.albumId);
                updatePlace.bind(3, sourceId);
                if (updatePlace.step() != StepResult::Done) {
                    return Status::databaseError("Failed to update album media membership: " + conn_.lastErrorMessage());
                }
            } else {
                auto delRes = conn_.prepare("DELETE FROM album_media WHERE album_id = ? AND media_id = ?;");
                if (!delRes.isOk()) return delRes.status();
                auto del = std::move(delRes.value());
                del.bind(1, ar.albumId);
                del.bind(2, sourceId);
                if (del.step() != StepResult::Done) {
                    return Status::databaseError("Failed to delete source album membership: " + conn_.lastErrorMessage());
                }

                auto compReadRes = conn_.prepare("SELECT media_id FROM album_media WHERE album_id = ? ORDER BY position ASC, media_id ASC;");
                if (!compReadRes.isOk()) return compReadRes.status();
                auto compRead = std::move(compReadRes.value());
                compRead.bind(1, ar.albumId);

                std::vector<MediaId> compactIds;
                StepResult compSr;
                while ((compSr = compRead.step()) == StepResult::Row) {
                    compactIds.push_back(compRead.getInt64(0));
                }
                if (compSr == StepResult::Error) {
                    return Status::databaseError("Failed to read album members for compaction: " + conn_.lastErrorMessage());
                }

                auto compUpRes = conn_.prepare("UPDATE album_media SET position = ? WHERE album_id = ? AND media_id = ?;");
                if (!compUpRes.isOk()) return compUpRes.status();
                auto compUp = std::move(compUpRes.value());
                for (size_t pos = 0; pos < compactIds.size(); ++pos) {
                    compUp.reset();
                    compUp.bind(1, static_cast<int64_t>(pos));
                    compUp.bind(2, ar.albumId);
                    compUp.bind(3, compactIds[pos]);
                    if (compUp.step() != StepResult::Done) {
                        return Status::databaseError("Failed to compact album media position: " + conn_.lastErrorMessage());
                    }
                }
            }
        }
    }

    // 8. Re-point covers: update albums where cover_media_id == sourceId to keeperId
    {
        auto coverRes = conn_.prepare("UPDATE albums SET cover_media_id = ? WHERE cover_media_id = ?;");
        if (!coverRes.isOk()) return coverRes.status();
        auto coverStmt = std::move(coverRes.value());
        coverStmt.bind(1, keeperId);
        coverStmt.bind(2, sourceId);
        if (coverStmt.step() != StepResult::Done) {
            return Status::databaseError("Failed to re-point album covers: " + conn_.lastErrorMessage());
        }
    }

    return tx.commit();
}

Status CatalogDb::runInTransaction(std::function<Status()> fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    if (!tx.isActive()) {
        return Status::databaseError("Failed to start transaction: " + conn_.lastErrorMessage());
    }
    Status s = fn();
    if (!s.isOk()) {
        tx.rollback();
        return s;
    }
    return tx.commit();
}

} // namespace imagine::db
