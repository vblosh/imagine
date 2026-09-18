#pragma once

#include <optional>
#include <string>
#include <vector>

#include "imagine/common/types.hpp"

namespace imagine::core {

enum class EventSuggestionNameSource {
    Event,
    Caption,
    Place,
    Folder,
    Date
};

struct EventSuggestionGroup {
    std::vector<MediaItem> photos;
    int64_t start_date{0};
    int64_t end_date{0};
    EventSuggestionNameSource name_source{EventSuggestionNameSource::Date};
    std::string name_value;
    std::optional<TagId> existing_event_tag_id;
    std::vector<std::string> people;
    std::vector<std::string> places;
    std::vector<std::string> keywords;
};

struct EventSuggestionSkipped {
    MediaId id{0};
    std::string reason;
};

struct EventSuggestionResult {
    std::vector<EventSuggestionGroup> groups;
    std::vector<EventSuggestionSkipped> skipped;
};

class EventSuggestionEngine {
public:
    static EventSuggestionResult suggest(const std::vector<MediaItem>& media);
};

const char* eventSuggestionNameSourceString(EventSuggestionNameSource source);

} // namespace imagine::core
