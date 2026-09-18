#include "imagine/core/event_suggestions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace imagine::core {

namespace {

constexpr int64_t kMaximumAdjacentGapSeconds = 4 * 60 * 60;
constexpr int64_t kMaximumGroupSpanSeconds = 12 * 60 * 60;
constexpr double kMaximumGpsGapKm = 25.0;
constexpr double kEarthRadiusKm = 6371.0088;

std::string lowerAscii(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (char ch : value) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower;
}

bool categoryEquals(const Tag& tag, std::string_view category) {
    return lowerAscii(tag.category) == category;
}

bool hasValidGps(const MediaItem& item) {
    return item.exif.has_gps &&
           std::isfinite(item.exif.latitude) && std::isfinite(item.exif.longitude) &&
           item.exif.latitude >= -90.0 && item.exif.latitude <= 90.0 &&
           item.exif.longitude >= -180.0 && item.exif.longitude <= 180.0;
}

double degreesToRadians(double degrees) {
    constexpr double kPi = 3.14159265358979323846;
    return degrees * kPi / 180.0;
}

double gpsDistanceKm(const MediaItem& a, const MediaItem& b) {
    const double lat1 = degreesToRadians(a.exif.latitude);
    const double lat2 = degreesToRadians(b.exif.latitude);
    const double dLat = lat2 - lat1;
    const double dLon = degreesToRadians(b.exif.longitude - a.exif.longitude);
    const double sinLat = std::sin(dLat / 2.0);
    const double sinLon = std::sin(dLon / 2.0);
    const double h = sinLat * sinLat + std::cos(lat1) * std::cos(lat2) * sinLon * sinLon;
    return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(std::min(1.0, h)));
}

std::vector<Tag> commonTags(const std::vector<MediaItem>& photos, std::string_view category) {
    if (photos.empty()) return {};

    std::map<TagId, Tag> common;
    for (const auto& tag : photos.front().tags) {
        if (categoryEquals(tag, category)) {
            common.emplace(tag.id, tag);
        }
    }

    for (size_t i = 1; i < photos.size() && !common.empty(); ++i) {
        std::unordered_set<TagId> ids;
        for (const auto& tag : photos[i].tags) {
            if (categoryEquals(tag, category)) ids.insert(tag.id);
        }
        for (auto it = common.begin(); it != common.end();) {
            if (!ids.contains(it->first)) {
                it = common.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<Tag> result;
    result.reserve(common.size());
    for (auto& [id, tag] : common) {
        (void)id;
        result.push_back(std::move(tag));
    }
    std::sort(result.begin(), result.end(), [](const Tag& a, const Tag& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.id < b.id;
    });
    return result;
}

std::vector<std::string> tagNames(const std::vector<Tag>& tags) {
    std::vector<std::string> names;
    names.reserve(tags.size());
    for (const auto& tag : tags) {
        if (!tag.name.empty() && (names.empty() || names.back() != tag.name)) {
            names.push_back(tag.name);
        }
    }
    return names;
}

std::optional<std::string> majorityCaption(const std::vector<MediaItem>& photos) {
    std::map<std::string, size_t> counts;
    for (const auto& photo : photos) {
        const std::string caption = trimWhitespace(photo.caption);
        if (!caption.empty()) ++counts[caption];
    }
    for (const auto& [caption, count] : counts) {
        if (count > photos.size() / 2) return caption;
    }
    return std::nullopt;
}

bool isGenericFolder(std::string_view folder) {
    static const std::unordered_set<std::string> generic = {
        "dcim", "camera", "pictures", "photos", "images"
    };
    const std::string trimmed = trimWhitespace(folder);
    if (trimmed.empty() || generic.contains(lowerAscii(trimmed))) return true;
    return std::all_of(trimmed.begin(), trimmed.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch)) != 0;
    });
}

std::optional<std::string> commonParentFolder(const std::vector<MediaItem>& photos) {
    std::optional<std::string> common;
    for (const auto& photo : photos) {
        const std::string folder = pathToUtf8(pathFromUtf8(photo.file_path).parent_path().filename());
        if (isGenericFolder(folder)) return std::nullopt;
        if (!common.has_value()) {
            common = folder;
        } else if (*common != folder) {
            return std::nullopt;
        }
    }
    return common;
}

EventSuggestionGroup makeGroup(std::vector<MediaItem> photos) {
    EventSuggestionGroup group;
    group.start_date = photos.front().date_taken;
    group.end_date = photos.back().date_taken;

    const auto people = commonTags(photos, "people");
    const auto places = commonTags(photos, "places");
    const auto keywords = commonTags(photos, "keyword");
    group.people = tagNames(people);
    group.places = tagNames(places);
    group.keywords = tagNames(keywords);

    const auto events = commonTags(photos, "events");
    if (!events.empty()) {
        group.name_source = EventSuggestionNameSource::Event;
        group.name_value = events.front().name;
        group.existing_event_tag_id = events.front().id;
    } else if (auto caption = majorityCaption(photos); caption.has_value()) {
        group.name_source = EventSuggestionNameSource::Caption;
        group.name_value = std::move(*caption);
    } else if (!places.empty()) {
        group.name_source = EventSuggestionNameSource::Place;
        group.name_value = places.front().name;
    } else if (auto folder = commonParentFolder(photos); folder.has_value()) {
        group.name_source = EventSuggestionNameSource::Folder;
        group.name_value = std::move(*folder);
    }

    group.photos = std::move(photos);
    return group;
}

} // namespace

EventSuggestionResult EventSuggestionEngine::suggest(const std::vector<MediaItem>& media) {
    EventSuggestionResult result;
    std::vector<MediaItem> eligible;
    eligible.reserve(media.size());

    for (const auto& item : media) {
        if (item.media_type != "photo") {
            result.skipped.push_back({item.id, "not_photo"});
        } else if (item.date_taken <= 0) {
            result.skipped.push_back({item.id, "invalid_date"});
        } else {
            eligible.push_back(item);
        }
    }

    std::sort(eligible.begin(), eligible.end(), [](const MediaItem& a, const MediaItem& b) {
        if (a.date_taken != b.date_taken) return a.date_taken < b.date_taken;
        return a.id < b.id;
    });

    std::vector<MediaItem> current;
    std::optional<MediaItem> previousLocated;
    for (auto& item : eligible) {
        bool split = false;
        if (!current.empty()) {
            split = item.date_taken - current.back().date_taken > kMaximumAdjacentGapSeconds ||
                    item.date_taken - current.front().date_taken > kMaximumGroupSpanSeconds;
            if (!split && previousLocated.has_value() && hasValidGps(item)) {
                split = gpsDistanceKm(*previousLocated, item) > kMaximumGpsGapKm;
            }
        }

        if (split) {
            result.groups.push_back(makeGroup(std::move(current)));
            current.clear();
            previousLocated.reset();
        }
        if (hasValidGps(item)) previousLocated = item;
        current.push_back(std::move(item));
    }
    if (!current.empty()) result.groups.push_back(makeGroup(std::move(current)));

    return result;
}

const char* eventSuggestionNameSourceString(EventSuggestionNameSource source) {
    switch (source) {
        case EventSuggestionNameSource::Event: return "event";
        case EventSuggestionNameSource::Caption: return "caption";
        case EventSuggestionNameSource::Place: return "place";
        case EventSuggestionNameSource::Folder: return "folder";
        case EventSuggestionNameSource::Date: return "date";
    }
    return "date";
}

} // namespace imagine::core
