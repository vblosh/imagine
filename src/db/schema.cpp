#include "imagine/db/schema.hpp"
#include "imagine/common/logger.hpp"

namespace imagine::db {

Result<int> Schema::getCurrentVersion(Connection& conn) {
    Status s = conn.execute("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY);");
    if (!s.isOk()) {
        return Result<int>::failure(s);
    }

    auto stmtResult = conn.prepare("SELECT MAX(version) FROM schema_version;");
    if (!stmtResult.isOk()) {
        return Result<int>::failure(stmtResult.status());
    }

    auto stmt = std::move(stmtResult.value());
    if (stmt.step() == StepResult::Row && !stmt.isNull(0)) {
        return stmt.getInt(0);
    }
    return 0;
}

Status Schema::applyMigrationV1(Connection& conn) {
    const char* v1_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS media_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path TEXT UNIQUE NOT NULL,
            file_name TEXT NOT NULL,
            file_size INTEGER NOT NULL,
            file_modified_time INTEGER NOT NULL,
            content_hash TEXT NOT NULL,
            width INTEGER DEFAULT 0,
            height INTEGER DEFAULT 0,
            date_taken INTEGER NOT NULL,
            date_taken_str TEXT DEFAULT '',
            rating INTEGER DEFAULT 0,
            flag INTEGER DEFAULT 0,
            camera_make TEXT DEFAULT '',
            camera_model TEXT DEFAULT '',
            lens TEXT DEFAULT '',
            exposure_time REAL DEFAULT 0.0,
            f_number REAL DEFAULT 0.0,
            iso INTEGER DEFAULT 0,
            focal_length REAL DEFAULT 0.0,
            orientation INTEGER DEFAULT 1,
            has_gps INTEGER DEFAULT 0,
            latitude REAL DEFAULT 0.0,
            longitude REAL DEFAULT 0.0,
            altitude REAL DEFAULT 0.0,
            thumb_small TEXT DEFAULT '',
            thumb_large TEXT DEFAULT '',
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_media_hash ON media_items(content_hash);
        CREATE INDEX IF NOT EXISTS idx_media_date ON media_items(date_taken DESC);
        CREATE INDEX IF NOT EXISTS idx_media_rating ON media_items(rating);
        CREATE INDEX IF NOT EXISTS idx_media_flag ON media_items(flag);
        CREATE INDEX IF NOT EXISTS idx_media_camera ON media_items(camera_make, camera_model);

        CREATE TABLE IF NOT EXISTS tags (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            category TEXT NOT NULL DEFAULT 'keyword',
            parent_id INTEGER REFERENCES tags(id) ON DELETE SET NULL,
            UNIQUE(name, category)
        );

        CREATE INDEX IF NOT EXISTS idx_tags_category ON tags(category);

        CREATE TABLE IF NOT EXISTS media_tags (
            media_id INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
            tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
            PRIMARY KEY(media_id, tag_id)
        );

        CREATE INDEX IF NOT EXISTS idx_media_tags_tag ON media_tags(tag_id);

        CREATE TABLE IF NOT EXISTS albums (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            description TEXT DEFAULT '',
            is_smart INTEGER DEFAULT 0,
            query_json TEXT DEFAULT '',
            cover_media_id INTEGER REFERENCES media_items(id) ON DELETE SET NULL,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS album_media (
            album_id INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
            media_id INTEGER NOT NULL REFERENCES media_items(id) ON DELETE CASCADE,
            position INTEGER DEFAULT 0,
            PRIMARY KEY(album_id, media_id)
        );

        CREATE INDEX IF NOT EXISTS idx_album_media_media ON album_media(media_id);
    )SQL";

    Transaction tx(conn);
    Status s = conn.execute(v1_sql);
    if (!s.isOk()) return s;

    s = conn.execute("INSERT INTO schema_version (version) VALUES (1);");
    if (!s.isOk()) return s;

    return tx.commit();
}

Status Schema::applyMigrationV2(Connection& conn) {
    const char* v2_sql = R"SQL(
        ALTER TABLE media_items ADD COLUMN caption TEXT DEFAULT '';
        CREATE INDEX IF NOT EXISTS idx_media_caption ON media_items(caption);
    )SQL";

    Transaction tx(conn);
    Status s = conn.execute(v2_sql);
    if (!s.isOk()) return s;

    s = conn.execute("INSERT INTO schema_version (version) VALUES (2);");
    if (!s.isOk()) return s;

    return tx.commit();
}

Status Schema::applyMigrationV3(Connection& conn) {
    const char* v3_sql = R"SQL(
        ALTER TABLE media_items ADD COLUMN media_type TEXT DEFAULT 'photo';
        ALTER TABLE media_items ADD COLUMN duration REAL DEFAULT 0.0;
        ALTER TABLE media_items ADD COLUMN audio_artist TEXT DEFAULT '';
        ALTER TABLE media_items ADD COLUMN audio_title TEXT DEFAULT '';
        ALTER TABLE media_items ADD COLUMN audio_album TEXT DEFAULT '';
        ALTER TABLE media_items ADD COLUMN audio_genre TEXT DEFAULT '';
        ALTER TABLE media_items ADD COLUMN codec TEXT DEFAULT '';
        ALTER TABLE media_items ADD COLUMN bitrate INTEGER DEFAULT 0;
        ALTER TABLE media_items ADD COLUMN channels INTEGER DEFAULT 0;
        ALTER TABLE media_items ADD COLUMN sample_rate INTEGER DEFAULT 0;

        CREATE INDEX IF NOT EXISTS idx_media_type ON media_items(media_type);
    )SQL";

    Transaction tx(conn);
    Status s = conn.execute(v3_sql);
    if (!s.isOk()) return s;

    s = conn.execute("INSERT INTO schema_version (version) VALUES (3);");
    if (!s.isOk()) return s;

    return tx.commit();
}

Status Schema::applyMigrationV4(Connection& conn) {
    const char* v4_sql = R"SQL(
        UPDATE media_items SET
            media_type = 'video',
            thumb_small = CASE WHEN (thumb_small = '' OR thumb_small IS NULL)
                          THEN substr(content_hash, 1, 2) || '/' || substr(content_hash, 3, 2) || '/' || content_hash || '_256.jpg'
                          ELSE thumb_small END,
            thumb_large = CASE WHEN (thumb_large = '' OR thumb_large IS NULL)
                          THEN substr(content_hash, 1, 2) || '/' || substr(content_hash, 3, 2) || '/' || content_hash || '_1024.jpg'
                          ELSE thumb_large END
        WHERE media_type = 'photo' AND (
            lower(file_name) LIKE '%.mp4' OR
            lower(file_name) LIKE '%.mov' OR
            lower(file_name) LIKE '%.avi' OR
            lower(file_name) LIKE '%.mkv' OR
            lower(file_name) LIKE '%.webm' OR
            lower(file_name) LIKE '%.m4v' OR
            lower(file_name) LIKE '%.mts' OR
            lower(file_name) LIKE '%.m2ts' OR
            lower(file_name) LIKE '%.m2t' OR
            lower(file_name) LIKE '%.mpg' OR
            lower(file_name) LIKE '%.mpeg' OR
            lower(file_name) LIKE '%.wmv' OR
            lower(file_name) LIKE '%.flv' OR
            lower(file_name) LIKE '%.3gp'
        );

        UPDATE media_items SET
            media_type = 'audio',
            thumb_small = CASE WHEN (thumb_small = '' OR thumb_small IS NULL)
                          THEN substr(content_hash, 1, 2) || '/' || substr(content_hash, 3, 2) || '/' || content_hash || '_256.jpg'
                          ELSE thumb_small END,
            thumb_large = CASE WHEN (thumb_large = '' OR thumb_large IS NULL)
                          THEN substr(content_hash, 1, 2) || '/' || substr(content_hash, 3, 2) || '/' || content_hash || '_1024.jpg'
                          ELSE thumb_large END
        WHERE media_type = 'photo' AND (
            lower(file_name) LIKE '%.mp3' OR
            lower(file_name) LIKE '%.wav' OR
            lower(file_name) LIKE '%.flac' OR
            lower(file_name) LIKE '%.ogg' OR
            lower(file_name) LIKE '%.m4a' OR
            lower(file_name) LIKE '%.aac' OR
            lower(file_name) LIKE '%.wma'
        );

        UPDATE media_items SET
            thumb_small = substr(content_hash, 1, 2) || '/' || substr(content_hash, 3, 2) || '/' || content_hash || '_256.jpg',
            thumb_large = substr(content_hash, 1, 2) || '/' || substr(content_hash, 3, 2) || '/' || content_hash || '_1024.jpg'
        WHERE media_type IN ('video', 'audio') AND (thumb_small = '' OR thumb_small IS NULL);
    )SQL";

    Transaction tx(conn);
    Status s = conn.execute(v4_sql);
    if (!s.isOk()) return s;

    s = conn.execute("INSERT INTO schema_version (version) VALUES (4);");
    if (!s.isOk()) return s;

    return tx.commit();
}

Status Schema::migrate(Connection& conn) {
    auto verResult = getCurrentVersion(conn);
    if (!verResult.isOk()) {
        return verResult.status();
    }

    int current = verResult.value();
    if (current < 1) {
        IMAGINE_LOG_INFO("Applying database migration v1...");
        Status s = applyMigrationV1(conn);
        if (!s.isOk()) return s;
        current = 1;
    }
    if (current < 2) {
        IMAGINE_LOG_INFO("Applying database migration v2...");
        Status s = applyMigrationV2(conn);
        if (!s.isOk()) return s;
        current = 2;
    }
    if (current < 3) {
        IMAGINE_LOG_INFO("Applying database migration v3...");
        Status s = applyMigrationV3(conn);
        if (!s.isOk()) return s;
        current = 3;
    }
    if (current < 4) {
        IMAGINE_LOG_INFO("Applying database migration v4...");
        Status s = applyMigrationV4(conn);
        if (!s.isOk()) return s;
        current = 4;
    }

    IMAGINE_LOG_INFO("Database schema up to date at version " + std::to_string(current));
    return Status::ok();
}

} // namespace imagine::db
