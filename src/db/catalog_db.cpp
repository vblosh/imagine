#include "imagine/db/catalog_db.hpp"
#include "imagine/db/schema.hpp"
#include "imagine/common/logger.hpp"
#include <chrono>
#include <set>
#include <unordered_set>

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
        WITH tag_covers AS (
            SELECT t.id AS tag_id,
                   (SELECT mt2.media_id
                    FROM media_tags mt2
                    JOIN media_items m2 ON mt2.media_id = m2.id
                    WHERE mt2.tag_id = t.id
                    ORDER BY (CASE WHEN m2.media_type IN ('video', 'audio') THEN 1 ELSE 0 END) ASC,
                             m2.date_taken DESC, m2.id DESC
                    LIMIT 1) AS cover_media_id
            FROM tags t
        )
        SELECT t.id, t.name, t.category, t.parent_id, COUNT(mt.media_id) AS media_count,
               tc.cover_media_id,
               cov.content_hash AS cover_hash,
               MIN(CASE WHEN m.date_taken > 0 THEN m.date_taken WHEN m.created_at > 0 THEN m.created_at WHEN m.file_modified_time > 0 THEN m.file_modified_time ELSE NULL END) AS first_date,
               MAX(CASE WHEN m.date_taken > 0 THEN m.date_taken WHEN m.created_at > 0 THEN m.created_at WHEN m.file_modified_time > 0 THEN m.file_modified_time ELSE NULL END) AS last_date
        FROM tags t
        LEFT JOIN tag_covers tc ON t.id = tc.tag_id
        LEFT JOIN media_items cov ON tc.cover_media_id = cov.id
        LEFT JOIN media_tags mt ON t.id = mt.tag_id
        LEFT JOIN media_items m ON mt.media_id = m.id
        GROUP BY t.id
        ORDER BY t.category ASC, t.name ASC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::vector<Tag> tags;
    while (stmt.step() == StepResult::Row) {
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
        tags.push_back(std::move(t));
    }
    return tags;
}

Result<std::vector<Tag>> CatalogDb::getTagsForMedia(MediaId id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = R"SQL(
        SELECT t.id, t.name, t.category, t.parent_id
        FROM tags t
        JOIN media_tags mt ON t.id = mt.tag_id
        WHERE mt.media_id = ?
        ORDER BY t.name ASC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, id);

    std::vector<Tag> tags;
    while (stmt.step() == StepResult::Row) {
        Tag t;
        t.id = stmt.getInt64(0);
        t.name = stmt.getString(1);
        t.category = stmt.getString(2);
        if (!stmt.isNull(3)) {
            t.parent_id = stmt.getInt64(3);
        }
        tags.push_back(std::move(t));
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
            SELECT mt.media_id, t.id, t.name, t.category, t.parent_id
            FROM tags t
            JOIN media_tags mt ON t.id = mt.tag_id
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

        while (stmt.step() == StepResult::Row) {
            MediaId mid = stmt.getInt64(0);
            Tag t;
            t.id = stmt.getInt64(1);
            t.name = stmt.getString(2);
            t.category = stmt.getString(3);
            if (!stmt.isNull(4)) {
                t.parent_id = stmt.getInt64(4);
            }
            result[mid].push_back(std::move(t));
        }
    }
    return result;
}

Status CatalogDb::addTagToMedia(MediaId mediaId, TagId tagId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    auto stmtRes = conn_.prepare("INSERT OR IGNORE INTO media_tags (media_id, tag_id) VALUES (?, ?);");
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());
    stmt.bind(1, mediaId);
    stmt.bind(2, tagId);

    if (stmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to add tag to media: " + conn_.lastErrorMessage());
    }
    auto provenanceRes = conn_.prepare(
        "INSERT INTO media_tag_provenance(media_id,tag_id,manual) VALUES(?,?,1) "
        "ON CONFLICT(media_id,tag_id) DO UPDATE SET manual=1;");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value());
    provenance.bind(1, mediaId); provenance.bind(2, tagId);
    if (provenance.step() != StepResult::Done) {
        return Status::databaseError("Failed to record manual tag assignment: " + conn_.lastErrorMessage());
    }
    return tx.commit();
}

Status CatalogDb::removeTagFromMedia(MediaId mediaId, TagId tagId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
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

    auto tagRes = createOrGetTag(name, category);
    if (!tagRes.isOk()) return tagRes.status();
    TagId tagId = tagRes.value();

    if (category == "people") {
        auto rejectRes = conn_.prepare(R"SQL(
            INSERT OR IGNORE INTO face_rejections(face_id,tag_id,rejected_at)
            SELECT id,?,? FROM faces WHERE person_tag_id=?;
        )SQL");
        if (!rejectRes.isOk()) return rejectRes.status();
        auto reject = std::move(rejectRes.value());
        reject.bind(1, tagId);
        reject.bind(2, currentUnixTime());
        reject.bind(3, tagId);
        if (reject.step() != StepResult::Done) {
            return Status::databaseError("Failed to record removed people identities: " + conn_.lastErrorMessage());
        }

        auto clearRes = conn_.prepare("UPDATE faces SET person_tag_id=NULL,revision=revision+1,updated_at=? WHERE person_tag_id=?;");
        if (!clearRes.isOk()) return clearRes.status();
        auto clear = std::move(clearRes.value());
        clear.bind(1, currentUnixTime());
        clear.bind(2, tagId);
        if (clear.step() != StepResult::Done) {
            return Status::databaseError("Failed to clear removed people identities: " + conn_.lastErrorMessage());
        }
    }

    auto deleteRes = conn_.prepare("DELETE FROM media_tags WHERE tag_id = ?;");
    if (!deleteRes.isOk()) return deleteRes.status();
    auto deleteStmt = std::move(deleteRes.value());
    deleteStmt.bind(1, tagId);
    if (deleteStmt.step() != StepResult::Done) {
        return Status::databaseError("Failed to clear tag assignments: " + conn_.lastErrorMessage());
    }

    auto clearProvenanceRes = conn_.prepare("DELETE FROM media_tag_provenance WHERE tag_id=?;");
    if (!clearProvenanceRes.isOk()) return clearProvenanceRes.status();
    auto clearProvenance = std::move(clearProvenanceRes.value()); clearProvenance.bind(1, tagId);
    if (clearProvenance.step() != StepResult::Done) return Status::databaseError("Failed to clear tag assignment provenance: " + conn_.lastErrorMessage());

    std::unordered_set<MediaId> uniqueMediaIds(mediaIds.begin(), mediaIds.end());

    auto insertRes = conn_.prepare("INSERT INTO media_tags (media_id, tag_id) VALUES (?, ?);");
    if (!insertRes.isOk()) return insertRes.status();
    auto insertStmt = std::move(insertRes.value());

    auto provenanceRes = conn_.prepare("INSERT INTO media_tag_provenance(media_id,tag_id,manual) VALUES(?,?,1);");
    if (!provenanceRes.isOk()) return provenanceRes.status();
    auto provenance = std::move(provenanceRes.value());

    for (MediaId mediaId : uniqueMediaIds) {
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
    while (stmt.step() == StepResult::Row) {
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
    while (membersStmt.step() == StepResult::Row) {
        memberIds.insert(membersStmt.getInt64(0));
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
    while (stmt.step() == StepResult::Row) {
        items.push_back(extractMediaItem(stmt));
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
        WHERE date_taken > 0
        GROUP BY y, m
        ORDER BY y DESC, m DESC;
    )SQL";

    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::vector<TimelineEntry> timeline;
    while (stmt.step() == StepResult::Row) {
        TimelineEntry entry;
        entry.year = stmt.getInt(0);
        entry.month = stmt.getInt(1);
        entry.count = stmt.getInt64(2);
        timeline.push_back(entry);
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
            COALESCE(MIN(CASE WHEN date_taken > 0 THEN date_taken END), 0),
            COALESCE(MAX(date_taken), 0),
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

    return s;
}

Result<std::vector<std::string>> CatalogDb::getAllFolders() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = "SELECT file_path FROM media_items;";
    auto stmtRes = conn_.prepare(sql);
    if (!stmtRes.isOk()) return stmtRes.status();
    auto stmt = std::move(stmtRes.value());

    std::set<std::string> folders;
    while (stmt.step() == StepResult::Row) {
        std::string path = stmt.getString(0);
        std::replace(path.begin(), path.end(), '\\', '/');
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != std::string::npos && lastSlash > 0) {
            folders.insert(path.substr(0, lastSlash));
        }
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
        stmt.bind(static_cast<int>(i + 1), params[i]);
    }

    std::vector<MediaItem> items;
    while (stmt.step() == StepResult::Row) {
        items.push_back(extractMediaItem(stmt));
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
        stmt.bind(static_cast<int>(i + 1), params[i]);
    }

    if (stmt.step() == StepResult::Row) {
        return stmt.getInt64(0);
    }
    return 0;
}

Result<int64_t> CatalogDb::makePathsRelative(const std::string& photosDir, const std::string& thumbsDir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_.isOpen()) {
        return Status::internal("Database not open");
    }

    std::error_code ec;
    std::filesystem::path normPhotos;
    if (!photosDir.empty()) {
        normPhotos = std::filesystem::weakly_canonical(photosDir, ec);
        if (ec) normPhotos = std::filesystem::path(photosDir).lexically_normal();
    }

    std::filesystem::path normThumbs;
    if (!thumbsDir.empty()) {
        normThumbs = std::filesystem::weakly_canonical(thumbsDir, ec);
        if (ec) normThumbs = std::filesystem::path(thumbsDir).lexically_normal();
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

    auto makeRelativePath = [&](const std::string& origPath, const std::filesystem::path& normBase) -> std::pair<std::string, bool> {
        if (origPath.empty()) {
            return {"", false};
        }
        std::filesystem::path p(origPath);
        std::string pathStr = origPath;
        bool changed = false;

        if (!normBase.empty()) {
            auto normalP = p.lexically_normal();
            auto rel = std::filesystem::relative(normalP, normBase, ec);
            if (!ec && !rel.empty()) {
                std::string relStr = rel.generic_string();
                if (relStr != ".." && relStr.rfind("../", 0) != 0 && relStr != ".") {
                    if (relStr != pathStr) {
                        pathStr = relStr;
                        changed = true;
                    }
                }
            }
        }
        std::string genericPath = std::filesystem::path(pathStr).generic_string();
        if (genericPath != pathStr) {
            pathStr = genericPath;
            changed = true;
        }
        return {pathStr, changed};
    };

    while (stmt.step() == StepResult::Row) {
        int64_t id = stmt.getInt64(0);
        std::string filePath = stmt.getString(1);
        std::string thumbSmall = stmt.getString(2);
        std::string thumbLarge = stmt.getString(3);

        auto [newFilePath, fpChanged] = makeRelativePath(filePath, normPhotos);
        auto [newThumbSmall, tsChanged] = makeRelativePath(thumbSmall, normThumbs);
        auto [newThumbLarge, tlChanged] = makeRelativePath(thumbLarge, normThumbs);

        if (fpChanged || tsChanged || tlChanged) {
            updates.push_back({id, newFilePath, newThumbSmall, newThumbLarge});
        }
    }

    if (updates.empty()) {
        return 0;
    }

    Transaction tx(conn_);
    auto updateStmtRes = conn_.prepare("UPDATE media_items SET file_path = ?, thumb_small = ?, thumb_large = ?, updated_at = ? WHERE id = ?;");
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

Status CatalogDb::runInTransaction(std::function<Status()> fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Transaction tx(conn_);
    Status s = fn();
    if (!s.isOk()) {
        tx.rollback();
        return s;
    }
    return tx.commit();
}

} // namespace imagine::db
