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
    if (ws.rfind(L"\\\\?\\UNC\\", 0) == 0 && ws.size() > 8) {
        return std::filesystem::path(L"\\\\" + ws.substr(8));
    }
    if (ws.rfind(L"\\\\?\\", 0) == 0 && ws.size() >= 6 && ws[5] == L':') {
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

inline std::filesystem::path pathFromUtf8(std::string_view utf8Str) {
#if defined(_WIN32)
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8Str.data()), utf8Str.size()));
#else
    return std::filesystem::path(utf8Str);
#endif
}

inline std::string sanitizeUtf8(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ) {
        const unsigned char byte = static_cast<unsigned char>(sv[i]);
        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte));
            ++i;
            continue;
        }
        const size_t length = (byte >= 0xC2 && byte <= 0xDF) ? 2
                            : (byte >= 0xE0 && byte <= 0xEF) ? 3
                            : (byte >= 0xF0 && byte <= 0xF4) ? 4 : 0;
        bool valid = (length != 0) && (length <= sv.size() - i);
        for (size_t j = 1; valid && j < length; ++j) {
            const unsigned char next = static_cast<unsigned char>(sv[i + j]);
            valid = (next >= 0x80 && next <= 0xBF);
            if (j == 1) {
                valid = valid && !(byte == 0xE0 && next < 0xA0)
                              && !(byte == 0xED && next >= 0xA0)
                              && !(byte == 0xF0 && next < 0x90)
                              && !(byte == 0xF4 && next >= 0x90);
            }
        }
        if (valid) {
            out.append(sv.substr(i, length));
            i += length;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

inline std::string trimWhitespace(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return "";
    }
    auto end = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(start, end - start + 1));
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
    std::string source;              // per-media provenance ("manual", "face", "clip")
    int64_t ai_count{0};             // tag-list metadata
    std::optional<TagId> parent_id{std::nullopt};
    int64_t media_count{0};
    std::optional<MediaId> cover_media_id{std::nullopt};
    std::string cover_hash;
    std::optional<int64_t> first_date{std::nullopt};
    std::optional<int64_t> last_date{std::nullopt};
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
    std::string media_type{"photo"}; // "photo", "video", "audio"
    double duration{0.0}; // in seconds
    std::string audio_artist;
    std::string audio_title;
    std::string audio_album;
    std::string audio_genre;
    std::string codec;
    int32_t bitrate{0};
    int32_t channels{0};
    int32_t sample_rate{0};
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
    int64_t total_photos{0};
    int64_t total_videos{0};
    int64_t total_audio{0};
    int64_t total_picks{0};
    int64_t total_rejects{0};
    int64_t total_not_rejects{0};
    int64_t total_unrated{0};
    double total_duration{0.0};
    int64_t total_size_bytes{0};
    int64_t total_tags{0};
    int64_t total_albums{0};
    int64_t earliest_date{0};
    int64_t latest_date{0};
    int64_t total_duplicates{0};
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
