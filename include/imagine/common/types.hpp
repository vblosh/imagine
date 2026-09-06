#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace imagine {

inline std::filesystem::path stripExtendedPrefix(const std::filesystem::path& p) {
#if defined(_WIN32)
    std::wstring ws = p.native();
    if (ws.rfind(L"\\\\?\\", 0) == 0 && ws.size() > 4 && ws[5] == L':') {
        return std::filesystem::path(ws.substr(4));
    }
#endif
    return p;
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
    auto cleaned = stripExtendedPrefix(p);
#if defined(_WIN32)
    auto u8 = cleaned.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return cleaned.string();
#endif
}

inline std::filesystem::path pathFromUtf8(const std::string& utf8Str) {
#if defined(_WIN32)
    return std::filesystem::path(std::u8string(utf8Str.begin(), utf8Str.end()));
#else
    return std::filesystem::path(utf8Str);
#endif
}

inline std::string sanitizeUtf8(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ) {
        unsigned char c = static_cast<unsigned char>(sv[i]);
        if (c < 0x80) {
            out.push_back(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < sv.size() && (static_cast<unsigned char>(sv[i + 1]) & 0xC0) == 0x80) {
                out.append(sv.substr(i, 2));
                i += 2;
            } else {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < sv.size() &&
                (static_cast<unsigned char>(sv[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(sv[i + 2]) & 0xC0) == 0x80) {
                out.append(sv.substr(i, 3));
                i += 3;
            } else {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < sv.size() &&
                (static_cast<unsigned char>(sv[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(sv[i + 2]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(sv[i + 3]) & 0xC0) == 0x80) {
                out.append(sv.substr(i, 4));
                i += 4;
            } else {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

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
    std::string caption;
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
