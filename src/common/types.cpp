#include "imagine/common/types.hpp"

namespace imagine {

void to_json(nlohmann::json& j, const ExifData& e) {
    j = nlohmann::json{
        {"camera_make", e.camera_make},
        {"camera_model", e.camera_model},
        {"lens", e.lens},
        {"date_taken", e.date_taken},
        {"date_taken_str", e.date_taken_str},
        {"exposure_time", e.exposure_time},
        {"f_number", e.f_number},
        {"iso", e.iso},
        {"focal_length", e.focal_length},
        {"orientation", e.orientation},
        {"has_gps", e.has_gps},
        {"latitude", e.latitude},
        {"longitude", e.longitude},
        {"altitude", e.altitude}
    };
}

void from_json(const nlohmann::json& j, ExifData& e) {
    e.camera_make = j.value("camera_make", "");
    e.camera_model = j.value("camera_model", "");
    e.lens = j.value("lens", "");
    e.date_taken = j.value("date_taken", int64_t{0});
    e.date_taken_str = j.value("date_taken_str", "");
    e.exposure_time = j.value("exposure_time", 0.0);
    e.f_number = j.value("f_number", 0.0);
    e.iso = j.value("iso", int32_t{0});
    e.focal_length = j.value("focal_length", 0.0);
    e.orientation = j.value("orientation", int32_t{1});
    e.has_gps = j.value("has_gps", false);
    e.latitude = j.value("latitude", 0.0);
    e.longitude = j.value("longitude", 0.0);
    e.altitude = j.value("altitude", 0.0);
}

void to_json(nlohmann::json& j, const Tag& t) {
    j = nlohmann::json{
        {"id", t.id},
        {"name", t.name},
        {"category", t.category},
        {"media_count", t.media_count},
        {"cover_hash", t.cover_hash}
    };
    if (t.parent_id.has_value()) {
        j["parent_id"] = *t.parent_id;
    } else {
        j["parent_id"] = nullptr;
    }
    if (t.cover_media_id.has_value()) {
        j["cover_media_id"] = *t.cover_media_id;
    } else {
        j["cover_media_id"] = nullptr;
    }
}

void from_json(const nlohmann::json& j, Tag& t) {
    t.id = j.value("id", TagId{0});
    t.name = j.value("name", "");
    t.category = j.value("category", "keyword");
    t.media_count = j.value("media_count", int64_t{0});
    t.cover_hash = j.value("cover_hash", "");
    if (j.contains("parent_id") && !j["parent_id"].is_null()) {
        t.parent_id = j["parent_id"].get<TagId>();
    } else {
        t.parent_id = std::nullopt;
    }
    if (j.contains("cover_media_id") && !j["cover_media_id"].is_null()) {
        t.cover_media_id = j["cover_media_id"].get<MediaId>();
    } else {
        t.cover_media_id = std::nullopt;
    }
}

void to_json(nlohmann::json& j, const MediaItem& m) {
    j = nlohmann::json{
        {"id", m.id},
        {"file_path", m.file_path},
        {"file_name", m.file_name},
        {"file_size", m.file_size},
        {"file_modified_time", m.file_modified_time},
        {"content_hash", m.content_hash},
        {"width", m.width},
        {"height", m.height},
        {"date_taken", m.date_taken},
        {"rating", m.rating},
        {"flag", static_cast<int8_t>(m.flag)},
        {"exif", m.exif},
        {"thumb_small", m.thumb_small},
        {"thumb_large", m.thumb_large},
        {"tags", m.tags},
        {"caption", m.caption},
        {"created_at", m.created_at},
        {"updated_at", m.updated_at},
        {"media_type", m.media_type},
        {"duration", m.duration},
        {"audio_artist", m.audio_artist},
        {"audio_title", m.audio_title},
        {"audio_album", m.audio_album},
        {"audio_genre", m.audio_genre},
        {"codec", m.codec},
        {"bitrate", m.bitrate},
        {"channels", m.channels},
        {"sample_rate", m.sample_rate}
    };
}

void from_json(const nlohmann::json& j, MediaItem& m) {
    m.id = j.value("id", MediaId{0});
    m.file_path = j.value("file_path", "");
    m.file_name = j.value("file_name", "");
    m.file_size = j.value("file_size", int64_t{0});
    m.file_modified_time = j.value("file_modified_time", int64_t{0});
    m.content_hash = j.value("content_hash", "");
    m.width = j.value("width", int32_t{0});
    m.height = j.value("height", int32_t{0});
    m.date_taken = j.value("date_taken", int64_t{0});
    m.rating = j.value("rating", int32_t{0});
    m.flag = static_cast<FlagState>(j.value("flag", int8_t{0}));
    if (j.contains("exif")) {
        m.exif = j["exif"].get<ExifData>();
    }
    m.thumb_small = j.value("thumb_small", "");
    m.thumb_large = j.value("thumb_large", "");
    if (j.contains("tags")) {
        m.tags = j["tags"].get<std::vector<Tag>>();
    }
    m.caption = j.value("caption", "");
    m.created_at = j.value("created_at", int64_t{0});
    m.updated_at = j.value("updated_at", int64_t{0});
    m.media_type = j.value("media_type", "photo");
    m.duration = j.value("duration", 0.0);
    m.audio_artist = j.value("audio_artist", "");
    m.audio_title = j.value("audio_title", "");
    m.audio_album = j.value("audio_album", "");
    m.audio_genre = j.value("audio_genre", "");
    m.codec = j.value("codec", "");
    m.bitrate = j.value("bitrate", int32_t{0});
    m.channels = j.value("channels", int32_t{0});
    m.sample_rate = j.value("sample_rate", int32_t{0});
}

void to_json(nlohmann::json& j, const Album& a) {
    j = nlohmann::json{
        {"id", a.id},
        {"name", a.name},
        {"description", a.description},
        {"is_smart", a.is_smart},
        {"query_json", a.query_json},
        {"item_count", a.item_count},
        {"created_at", a.created_at}
    };
    if (a.cover_media_id.has_value()) {
        j["cover_media_id"] = *a.cover_media_id;
    } else {
        j["cover_media_id"] = nullptr;
    }
}

void from_json(const nlohmann::json& j, Album& a) {
    a.id = j.value("id", AlbumId{0});
    a.name = j.value("name", "");
    a.description = j.value("description", "");
    a.is_smart = j.value("is_smart", false);
    a.query_json = j.value("query_json", "");
    a.item_count = j.value("item_count", int64_t{0});
    a.created_at = j.value("created_at", int64_t{0});
    if (j.contains("cover_media_id") && !j["cover_media_id"].is_null()) {
        a.cover_media_id = j["cover_media_id"].get<MediaId>();
    } else {
        a.cover_media_id = std::nullopt;
    }
}

void to_json(nlohmann::json& j, const CatalogStats& s) {
    j = nlohmann::json{
        {"total_media", s.total_media},
        {"total_photos", s.total_photos},
        {"total_videos", s.total_videos},
        {"total_audio", s.total_audio},
        {"total_picks", s.total_picks},
        {"total_rejects", s.total_rejects},
        {"total_not_rejects", s.total_not_rejects},
        {"total_unrated", s.total_unrated},
        {"total_duration", s.total_duration},
        {"total_size_bytes", s.total_size_bytes},
        {"total_tags", s.total_tags},
        {"total_albums", s.total_albums},
        {"earliest_date", s.earliest_date},
        {"latest_date", s.latest_date}
    };
}

void from_json(const nlohmann::json& j, CatalogStats& s) {
    s.total_media = j.value("total_media", int64_t{0});
    s.total_photos = j.value("total_photos", int64_t{0});
    s.total_videos = j.value("total_videos", int64_t{0});
    s.total_audio = j.value("total_audio", int64_t{0});
    s.total_picks = j.value("total_picks", int64_t{0});
    s.total_rejects = j.value("total_rejects", int64_t{0});
    s.total_not_rejects = j.value("total_not_rejects", int64_t{0});
    s.total_unrated = j.value("total_unrated", int64_t{0});
    s.total_duration = j.value("total_duration", 0.0);
    s.total_size_bytes = j.value("total_size_bytes", int64_t{0});
    s.total_tags = j.value("total_tags", int64_t{0});
    s.total_albums = j.value("total_albums", int64_t{0});
    s.earliest_date = j.value("earliest_date", int64_t{0});
    s.latest_date = j.value("latest_date", int64_t{0});
}

void to_json(nlohmann::json& j, const TimelineEntry& t) {
    j = nlohmann::json{
        {"year", t.year},
        {"month", t.month},
        {"count", t.count}
    };
}

void from_json(const nlohmann::json& j, TimelineEntry& t) {
    t.year = j.value("year", int32_t{0});
    t.month = j.value("month", int32_t{0});
    t.count = j.value("count", int64_t{0});
}

} // namespace imagine
