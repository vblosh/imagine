#include "imagine/common/types.hpp"

namespace imagine {

namespace {

std::string jsonStringOr(const nlohmann::json& j, const char* key, const char* defaultVal = "") {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return defaultVal;
}

template <typename IntT>
IntT jsonIntOr(const nlohmann::json& j, const char* key, IntT defaultVal = 0) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) {
        return it->get<IntT>();
    }
    return defaultVal;
}

double jsonDoubleOr(const nlohmann::json& j, const char* key, double defaultVal = 0.0) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        return it->get<double>();
    }
    return defaultVal;
}

bool jsonBoolOr(const nlohmann::json& j, const char* key, bool defaultVal = false) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) {
        return it->get<bool>();
    }
    return defaultVal;
}

} // namespace

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
    e.camera_make = jsonStringOr(j, "camera_make", "");
    e.camera_model = jsonStringOr(j, "camera_model", "");
    e.lens = jsonStringOr(j, "lens", "");
    e.date_taken = jsonIntOr<int64_t>(j, "date_taken", int64_t{0});
    e.date_taken_str = jsonStringOr(j, "date_taken_str", "");
    e.exposure_time = jsonDoubleOr(j, "exposure_time", 0.0);
    e.f_number = jsonDoubleOr(j, "f_number", 0.0);
    e.iso = jsonIntOr<int32_t>(j, "iso", int32_t{0});
    e.focal_length = jsonDoubleOr(j, "focal_length", 0.0);
    e.orientation = jsonIntOr<int32_t>(j, "orientation", int32_t{1});
    e.has_gps = jsonBoolOr(j, "has_gps", false);
    e.latitude = jsonDoubleOr(j, "latitude", 0.0);
    e.longitude = jsonDoubleOr(j, "longitude", 0.0);
    e.altitude = jsonDoubleOr(j, "altitude", 0.0);
}

void to_json(nlohmann::json& j, const Tag& t) {
    j = nlohmann::json{
        {"id", t.id},
        {"name", t.name},
        {"category", t.category},
        {"ai_count", t.ai_count},
        {"media_count", t.media_count},
        {"cover_hash", t.cover_hash}
    };
    if (!t.source.empty()) {
        j["source"] = t.source;
    }
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
    if (t.first_date.has_value()) {
        j["first_date"] = *t.first_date;
    } else {
        j["first_date"] = nullptr;
    }
    if (t.last_date.has_value()) {
        j["last_date"] = *t.last_date;
    } else {
        j["last_date"] = nullptr;
    }
}

void from_json(const nlohmann::json& j, Tag& t) {
    t.id = jsonIntOr<TagId>(j, "id", TagId{0});
    t.name = jsonStringOr(j, "name", "");
    t.category = jsonStringOr(j, "category", "keyword");
    t.source = jsonStringOr(j, "source", "");
    t.ai_count = jsonIntOr<int64_t>(j, "ai_count", int64_t{0});
    t.media_count = jsonIntOr<int64_t>(j, "media_count", int64_t{0});
    t.cover_hash = jsonStringOr(j, "cover_hash", "");
    if (j.contains("parent_id") && j["parent_id"].is_number_integer()) {
        t.parent_id = j["parent_id"].get<TagId>();
    } else {
        t.parent_id = std::nullopt;
    }
    if (j.contains("cover_media_id") && j["cover_media_id"].is_number_integer()) {
        t.cover_media_id = j["cover_media_id"].get<MediaId>();
    } else {
        t.cover_media_id = std::nullopt;
    }
    if (j.contains("first_date") && j["first_date"].is_number_integer()) {
        t.first_date = j["first_date"].get<int64_t>();
    } else {
        t.first_date = std::nullopt;
    }
    if (j.contains("last_date") && j["last_date"].is_number_integer()) {
        t.last_date = j["last_date"].get<int64_t>();
    } else {
        t.last_date = std::nullopt;
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
    m.id = jsonIntOr<MediaId>(j, "id", MediaId{0});
    m.file_path = jsonStringOr(j, "file_path", "");
    m.file_name = jsonStringOr(j, "file_name", "");
    m.file_size = jsonIntOr<int64_t>(j, "file_size", int64_t{0});
    m.file_modified_time = jsonIntOr<int64_t>(j, "file_modified_time", int64_t{0});
    m.content_hash = jsonStringOr(j, "content_hash", "");
    m.width = jsonIntOr<int32_t>(j, "width", int32_t{0});
    m.height = jsonIntOr<int32_t>(j, "height", int32_t{0});
    m.date_taken = jsonIntOr<int64_t>(j, "date_taken", int64_t{0});
    m.rating = jsonIntOr<int32_t>(j, "rating", int32_t{0});
    const int8_t rawFlag = jsonIntOr<int8_t>(j, "flag", int8_t{0});
    if (rawFlag == static_cast<int8_t>(FlagState::Reject) ||
        rawFlag == static_cast<int8_t>(FlagState::Unflagged) ||
        rawFlag == static_cast<int8_t>(FlagState::Pick)) {
        m.flag = static_cast<FlagState>(rawFlag);
    } else {
        m.flag = FlagState::Unflagged;
    }
    if (j.contains("exif") && j["exif"].is_object()) {
        m.exif = j["exif"].get<ExifData>();
    } else {
        m.exif = ExifData{};
    }
    m.thumb_small = jsonStringOr(j, "thumb_small", "");
    m.thumb_large = jsonStringOr(j, "thumb_large", "");
    if (j.contains("tags") && j["tags"].is_array()) {
        m.tags = j["tags"].get<std::vector<Tag>>();
    } else {
        m.tags.clear();
    }
    m.caption = jsonStringOr(j, "caption", "");
    m.created_at = jsonIntOr<int64_t>(j, "created_at", int64_t{0});
    m.updated_at = jsonIntOr<int64_t>(j, "updated_at", int64_t{0});
    m.media_type = jsonStringOr(j, "media_type", "photo");
    m.duration = jsonDoubleOr(j, "duration", 0.0);
    m.audio_artist = jsonStringOr(j, "audio_artist", "");
    m.audio_title = jsonStringOr(j, "audio_title", "");
    m.audio_album = jsonStringOr(j, "audio_album", "");
    m.audio_genre = jsonStringOr(j, "audio_genre", "");
    m.codec = jsonStringOr(j, "codec", "");
    m.bitrate = jsonIntOr<int32_t>(j, "bitrate", int32_t{0});
    m.channels = jsonIntOr<int32_t>(j, "channels", int32_t{0});
    m.sample_rate = jsonIntOr<int32_t>(j, "sample_rate", int32_t{0});
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
    a.id = jsonIntOr<AlbumId>(j, "id", AlbumId{0});
    a.name = jsonStringOr(j, "name", "");
    a.description = jsonStringOr(j, "description", "");
    a.is_smart = jsonBoolOr(j, "is_smart", false);
    a.query_json = jsonStringOr(j, "query_json", "");
    a.item_count = jsonIntOr<int64_t>(j, "item_count", int64_t{0});
    a.created_at = jsonIntOr<int64_t>(j, "created_at", int64_t{0});
    if (j.contains("cover_media_id") && j["cover_media_id"].is_number_integer()) {
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
        {"latest_date", s.latest_date},
        {"total_duplicates", s.total_duplicates}
    };
}

void from_json(const nlohmann::json& j, CatalogStats& s) {
    s.total_media = jsonIntOr<int64_t>(j, "total_media", int64_t{0});
    s.total_photos = jsonIntOr<int64_t>(j, "total_photos", int64_t{0});
    s.total_videos = jsonIntOr<int64_t>(j, "total_videos", int64_t{0});
    s.total_audio = jsonIntOr<int64_t>(j, "total_audio", int64_t{0});
    s.total_picks = jsonIntOr<int64_t>(j, "total_picks", int64_t{0});
    s.total_rejects = jsonIntOr<int64_t>(j, "total_rejects", int64_t{0});
    s.total_not_rejects = jsonIntOr<int64_t>(j, "total_not_rejects", int64_t{0});
    s.total_unrated = jsonIntOr<int64_t>(j, "total_unrated", int64_t{0});
    s.total_duration = jsonDoubleOr(j, "total_duration", 0.0);
    s.total_size_bytes = jsonIntOr<int64_t>(j, "total_size_bytes", int64_t{0});
    s.total_tags = jsonIntOr<int64_t>(j, "total_tags", int64_t{0});
    s.total_albums = jsonIntOr<int64_t>(j, "total_albums", int64_t{0});
    s.earliest_date = jsonIntOr<int64_t>(j, "earliest_date", int64_t{0});
    s.latest_date = jsonIntOr<int64_t>(j, "latest_date", int64_t{0});
    s.total_duplicates = jsonIntOr<int64_t>(j, "total_duplicates", int64_t{0});
}

void to_json(nlohmann::json& j, const TimelineEntry& t) {
    j = nlohmann::json{
        {"year", t.year},
        {"month", t.month},
        {"count", t.count}
    };
}

void from_json(const nlohmann::json& j, TimelineEntry& t) {
    t.year = jsonIntOr<int32_t>(j, "year", int32_t{0});
    t.month = jsonIntOr<int32_t>(j, "month", int32_t{0});
    t.count = jsonIntOr<int64_t>(j, "count", int64_t{0});
}

} // namespace imagine
