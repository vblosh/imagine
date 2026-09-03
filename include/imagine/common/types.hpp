#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace imagine {

using MediaId = int64_t;
using TagId = int64_t;
using AlbumId = int64_t;

enum class FlagState : int8_t {
    Reject = -1,
    Unflagged = 0,
    Pick = 1
};

struct ExifData {
    std::string camera_make;
    std::string camera_model;
    std::string lens;
    int64_t date_taken{0}; // Unix timestamp in seconds
    std::string date_taken_str;
    double exposure_time{0.0};
    double f_number{0.0};
    int32_t iso{0};
    double focal_length{0.0};
    int32_t orientation{1}; // EXIF 1-8
    bool has_gps{false};
    double latitude{0.0};
    double longitude{0.0};
    double altitude{0.0};
};

struct Tag {
    TagId id{0};
    std::string name;
    std::string category{"keyword"}; // "people", "places", "events", "keyword"
    std::optional<TagId> parent_id{std::nullopt};
    int64_t media_count{0};
};

struct MediaItem {
    MediaId id{0};
    std::string file_path;
    std::string file_name;
    int64_t file_size{0};
    int64_t file_modified_time{0};
    std::string content_hash;
    int32_t width{0};
    int32_t height{0};
    int64_t date_taken{0};
    int32_t rating{0}; // 0 - 5
    FlagState flag{FlagState::Unflagged};
    ExifData exif;
    std::string thumb_small; // path or relative URL
    std::string thumb_large; // path or relative URL
    std::vector<Tag> tags;
    int64_t created_at{0};
    int64_t updated_at{0};
};

struct Album {
    AlbumId id{0};
    std::string name;
    std::string description;
    bool is_smart{false};
    std::string query_json;
    std::optional<MediaId> cover_media_id{std::nullopt};
    int64_t item_count{0};
    int64_t created_at{0};
};

struct CatalogStats {
    int64_t total_media{0};
    int64_t total_size_bytes{0};
    int64_t total_tags{0};
    int64_t total_albums{0};
    int64_t earliest_date{0};
    int64_t latest_date{0};
};

struct TimelineEntry {
    int32_t year{0};
    int32_t month{0}; // 1 - 12
    int64_t count{0};
};

// JSON conversions
void to_json(nlohmann::json& j, const ExifData& e);
void from_json(const nlohmann::json& j, ExifData& e);

void to_json(nlohmann::json& j, const Tag& t);
void from_json(const nlohmann::json& j, Tag& t);

void to_json(nlohmann::json& j, const MediaItem& m);
void from_json(const nlohmann::json& j, MediaItem& m);

void to_json(nlohmann::json& j, const Album& a);
void from_json(const nlohmann::json& j, Album& a);

void to_json(nlohmann::json& j, const CatalogStats& s);
void from_json(const nlohmann::json& j, CatalogStats& s);

void to_json(nlohmann::json& j, const TimelineEntry& t);
void from_json(const nlohmann::json& j, TimelineEntry& t);

} // namespace imagine
